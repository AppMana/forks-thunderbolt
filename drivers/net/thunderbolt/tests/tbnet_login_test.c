// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for thunderbolt_net's login handshake after adopting the shared
 * struct tb_xdomain_handshake (the negotiation header shared with the core and
 * the RDMA stack). Verifies the soft-reconnect contract main.c relies on:
 * connected == handshake complete, and an inbound LOGIN arriving while already
 * complete supersedes the stale session (main.c then tears the tunnel down and
 * the peer's login retries re-establish a fresh one) instead of carrying
 * traffic into the peer's freed rings.
 *
 * Built into the module only on a CONFIG_KUNIT kernel (see Kconfig/Makefile).
 * The fleet's Ubuntu generic kernels lack CONFIG_KUNIT, so the same cases run
 * on-host via the userspace mirror tests/tbnet_login_userspace.c -- keep both
 * in lockstep.
 */
#include <kunit/test.h>

#include "../../../thunderbolt/thunderbolt_negotiation.h"

bool tbnet_test_session_needs_teardown(bool handshake_complete,
				       bool session_active);

static void tbnet_test_login_connect(struct kunit *test)
{
	struct tb_xdomain_handshake hs = { 0 };

	/* main.c: login sent -> request_sent, remote login -> peer_seen */
	hs.request_sent = true;
	hs.peer_seen = true;
	/* connected == tbnet_connected() == handshake complete */
	KUNIT_EXPECT_TRUE(test, tb_xdomain_handshake_complete(&hs));
}

static void tbnet_test_login_supersede_on_relogin(struct kunit *test)
{
	struct tb_xdomain_handshake hs = { 0 };

	/* established connection */
	hs.request_sent = true;
	hs.peer_seen = true;
	KUNIT_EXPECT_TRUE(test, tb_xdomain_handshake_complete(&hs));

	/*
	 * A fresh inbound LOGIN while complete == the peer restarted its side
	 * without a link edge. main.c's TBIP_LOGIN handler supersedes and queues
	 * disconnect_work; here we assert the handshake-level effect: it reports
	 * a supersede and is no longer complete until re-confirmed.
	 */
	KUNIT_EXPECT_TRUE(test, tb_xdomain_handshake_supersede(&hs));
	KUNIT_EXPECT_FALSE(test, tb_xdomain_handshake_complete(&hs));

	/* a duplicate LOGIN with no live session must NOT report a supersede */
	KUNIT_EXPECT_FALSE(test, tb_xdomain_handshake_supersede(&hs));
}

static void tbnet_test_login_reconnect(struct kunit *test)
{
	struct tb_xdomain_handshake hs = { 0 };

	hs.request_sent = true;
	hs.peer_seen = true;
	tb_xdomain_handshake_supersede(&hs);

	/* tear-down resets (start_login/tbnet_tear_down), then re-handshake */
	tb_xdomain_handshake_reset(&hs);
	KUNIT_EXPECT_FALSE(test, tb_xdomain_handshake_complete(&hs));
	hs.request_sent = true;
	hs.peer_seen = true;
	KUNIT_EXPECT_TRUE(test, tb_xdomain_handshake_complete(&hs));
}

static void tbnet_test_supersede_still_tears_down_owned_session(struct kunit *test)
{
	struct tb_xdomain_handshake hs = {
		.request_sent = true,
		.peer_seen = true,
	};
	bool session_active = true;

	KUNIT_ASSERT_TRUE(test, tb_xdomain_handshake_supersede(&hs));
	KUNIT_ASSERT_FALSE(test, tb_xdomain_handshake_complete(&hs));

	/* Resource ownership survives the control-state reset. */
	KUNIT_EXPECT_TRUE(test,
		tbnet_test_session_needs_teardown(
			tb_xdomain_handshake_complete(&hs), session_active));
}

/*
 * Session-verify (zombie) model. Lockstep lever with main.c: 1 = the driver
 * runs tbnet_verify_work() while carrier is on (the shipped fix), 0 = model
 * the pre-fix driver whose only post-carrier code paths are the two
 * early-returns (tbnet_login_work() and tbnet_connected_work() both bail on
 * netif_carrier_ok) -- the appmana-018<->027 mutual zombie.
 */
#ifndef TBNET_SESSION_VERIFY
#define TBNET_SESSION_VERIFY 1
#endif

/*
 * Minimal model of one tbnet netdev's post-open machinery, mirroring main.c:
 * login_work / connected_work carrier gates, tear_down, and (levered) the
 * session-verify work. @paths_active mirrors tb_xdomain_paths_active(): the
 * DMA tunnel is programmed and its hop entries read back enable=1.
 */
struct tbnet_model {
	struct tb_xdomain_handshake hs;
	bool carrier;
	bool paths_active;
	unsigned int logins_sent;
};

static void tbnet_model_login_work(struct tbnet_model *m)
{
	if (m->carrier)		/* main.c:tbnet_login_work() carrier gate */
		return;
	m->logins_sent++;
	m->hs.request_sent = true;	/* reply received */
}

static void tbnet_model_peer_login(struct tbnet_model *m)
{
	m->hs.peer_seen = true;		/* main.c:tbnet_handle_packet(LOGIN) */
}

static void tbnet_model_connected_work(struct tbnet_model *m)
{
	if (m->carrier)		/* main.c:tbnet_connected_work() carrier gate */
		return;
	if (!tb_xdomain_handshake_complete(&m->hs))
		return;
	m->paths_active = true;		/* tb_xdomain_enable_paths() */
	m->carrier = true;		/* netif_carrier_on() */
}

static void tbnet_model_tear_down(struct tbnet_model *m)
{
	m->carrier = false;
	m->paths_active = false;
	tb_xdomain_handshake_reset(&m->hs);
}

static void tbnet_model_verify_tick(struct tbnet_model *m)
{
	if (!TBNET_SESSION_VERIFY)
		return;			/* pre-fix driver: no verify work */
	if (!m->carrier)
		return;
	if (!tb_xdomain_session_zombie(&m->hs, m->paths_active))
		return;
	tbnet_model_tear_down(m);	/* main.c:tbnet_verify_work() */
	tbnet_model_login_work(m);	/* start_login() re-runs the spec flow */
}

static void tbnet_model_establish(struct tbnet_model *m)
{
	tbnet_model_login_work(m);
	tbnet_model_peer_login(m);
	tbnet_model_connected_work(m);
}

static void tbnet_test_session_zombie_recovers(struct kunit *test)
{
	struct tbnet_model m = { 0 };

	tbnet_model_establish(&m);
	KUNIT_EXPECT_TRUE(test, m.carrier);
	KUNIT_EXPECT_TRUE(test, m.paths_active);

	/*
	 * Peer reboots/re-negotiates WITHOUT a processed hotplug edge: the DMA
	 * tunnel dies underneath the session (hardware-observed as hop entries
	 * with enable=0) while carrier stays latched on.
	 */
	m.paths_active = false;

	/* The pre-fix driver's only post-carrier work: both gates bail. */
	tbnet_model_login_work(&m);
	tbnet_model_connected_work(&m);
	KUNIT_EXPECT_TRUE(test, m.carrier);
	KUNIT_EXPECT_FALSE(test, m.paths_active);

	/*
	 * Level-triggered revalidation must notice the zombie, tear the
	 * session down and re-run LOGIN. RED with TBNET_SESSION_VERIFY=0:
	 * carrier stays up forever on a dead tunnel and no LOGIN is ever sent
	 * again -- exactly the live tb-ch2/tb-ch1 zombie.
	 */
	tbnet_model_verify_tick(&m);
	KUNIT_EXPECT_FALSE(test, m.carrier);
	KUNIT_EXPECT_EQ(test, 2u, m.logins_sent);

	/* The peer's login retries then re-establish a fresh session. */
	tbnet_model_peer_login(&m);
	tbnet_model_connected_work(&m);
	KUNIT_EXPECT_TRUE(test, m.carrier);
	KUNIT_EXPECT_TRUE(test, m.paths_active);
}

static void tbnet_test_session_verify_healthy_noop(struct kunit *test)
{
	struct tbnet_model m = { 0 };
	int i;

	tbnet_model_establish(&m);

	/* A healthy established session must never be churned by verify. */
	for (i = 0; i < 5; i++) {
		tbnet_model_verify_tick(&m);
		KUNIT_EXPECT_TRUE(test, m.carrier);
		KUNIT_EXPECT_TRUE(test, m.paths_active);
	}
	KUNIT_EXPECT_EQ(test, 1u, m.logins_sent);

	/* Mid-handshake (not established) is the login loop's business. */
	tbnet_model_tear_down(&m);
	m.hs.request_sent = true;	/* login sent, peer not seen yet */
	tbnet_model_verify_tick(&m);
	KUNIT_EXPECT_FALSE(test, m.carrier);
	KUNIT_EXPECT_TRUE(test, m.hs.request_sent);
}

static struct kunit_case tbnet_login_test_cases[] = {
	KUNIT_CASE(tbnet_test_login_connect),
	KUNIT_CASE(tbnet_test_login_supersede_on_relogin),
	KUNIT_CASE(tbnet_test_login_reconnect),
	KUNIT_CASE(tbnet_test_supersede_still_tears_down_owned_session),
	KUNIT_CASE(tbnet_test_session_zombie_recovers),
	KUNIT_CASE(tbnet_test_session_verify_healthy_noop),
	{}
};

static struct kunit_suite tbnet_login_test_suite = {
	.name = "thunderbolt_net_login",
	.test_cases = tbnet_login_test_cases,
};
kunit_test_suite(tbnet_login_test_suite);
