// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for thunderbolt_net's login handshake after adopting the shared
 * struct tb_xdomain_handshake (the negotiation header shared with the core and
 * thunderbolt_ibverbs). Verifies the soft-reconnect contract main.c relies on:
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

static struct kunit_case tbnet_login_test_cases[] = {
	KUNIT_CASE(tbnet_test_login_connect),
	KUNIT_CASE(tbnet_test_login_supersede_on_relogin),
	KUNIT_CASE(tbnet_test_login_reconnect),
	{}
};

static struct kunit_suite tbnet_login_test_suite = {
	.name = "thunderbolt_net_login",
	.test_cases = tbnet_login_test_cases,
};
kunit_test_suite(tbnet_login_test_suite);
