// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for thunderbolt_ibverbs native ACK/control-frame routing and PSN
 * arithmetic. These encode driver-behaviour contracts that regressed silently
 * and cost a long debugging session, so they are documented in code:
 *
 *  - tbv_ack_route_peer: an ACK is a RESPONSE to a request that arrived on
 *    rx_path, so it MUST route back to rx_path's peer (the requester), NOT the
 *    QP's bound peer. On a mid-chain host cabled to two neighbours the responder
 *    QP is round-robin rail-bound at creation (before any request), while
 *    inbound requests are demuxed by QPN regardless of binding. Routing ACKs by
 *    the QP's bound peer sent them out the wrong neighbour's rail: writes flowed
 *    forward but ACKs never returned -> IBV_WC_RETRY_EXC_ERR. Reproduced on
 *    appmana-020<->009; fixed in tbv_select_native_control_path_for_qp_locked /
 *    tbv_send_ack_on_path.
 *
 *  - tbv_psn_delta: signed 24-bit PSN distance with wraparound, used to decide
 *    in-window vs duplicate ACKs.
 *
 * Build: included in the module only when CONFIG_KUNIT is set. Run via
 * drivers/thunderbolt_ibverbs/tools/run-kunit.sh (kunit.py on an overlaid tree).
 */
#include <kunit/test.h>
#include "../tbv.h"
#include "negotiation_model.h"

/*
 * Coordinated reload of a two-host link using the shared negotiation model
 * (mocked ICM firmware + gen gate + login/HELLO handshake). Returns whether the
 * link is established after `steps`. Same model as forks-thunderbolt's
 * tb_test_xdomain_negotiation_hang -- the ibverbs native READY handshake now
 * uses the same struct tb_xdomain_handshake + re-arm contract.
 */
static bool tbv_model_coord_reload(bool fix, int settle, int budget, int steps)
{
	struct model_link L;

	memset(&L, 0, sizeof(L));
	model_host_boot(&L.a, fix, settle, budget);
	model_host_boot(&L.b, fix, settle, budget);
	model_run(&L, 20);
	if (!model_established(&L))
		return false;
	model_host_reload(&L.a);
	model_host_reload(&L.b);
	model_run(&L, steps);
	return model_established(&L);
}

/* Adversarial: the hang is conditional on budget<settle and permanent; the
 * handshake re-arm fix recovers across a wide settle sweep. */
static void tbv_test_native_handshake_hang(struct kunit *test)
{
	int settle;

	KUNIT_EXPECT_FALSE(test, tbv_model_coord_reload(false, 5, 3, 200));
	KUNIT_EXPECT_FALSE(test, tbv_model_coord_reload(false, 4, 3, 200));
	KUNIT_EXPECT_TRUE(test, tbv_model_coord_reload(false, 3, 3, 200));
	KUNIT_EXPECT_TRUE(test, tbv_model_coord_reload(false, 2, 3, 200));
	KUNIT_EXPECT_FALSE(test, tbv_model_coord_reload(false, 5, 3, 5000));
	for (settle = 0; settle <= 40; settle++)
		KUNIT_EXPECT_TRUE(test, tbv_model_coord_reload(true, settle, 3, 200));
}

/* multi-rail: an ibverbs peer's rails (lanes) share one property generation */
static bool tbv_model_mcoord(int nrails, bool fix, int settle, int budget, int steps)
{
	struct model_mlink M;

	memset(&M, 0, sizeof(M));
	model_mhost_boot(&M.a, nrails, fix, settle, budget);
	model_mhost_boot(&M.b, nrails, fix, settle, budget);
	model_mrun(&M, 20);
	if (!model_mestablished(&M))
		return false;
	model_mhost_reload(&M.a);
	model_mhost_reload(&M.b);
	model_mrun(&M, steps);
	return model_mestablished(&M);
}

static void tbv_test_native_handshake_multirail(struct kunit *test)
{
	int nrails, settle;

	for (nrails = 1; nrails <= MODEL_MAX_RAILS; nrails++)
		KUNIT_EXPECT_FALSE(test, tbv_model_mcoord(nrails, false, 5, 3, 200));

	for (nrails = 1; nrails <= MODEL_MAX_RAILS; nrails++)
		for (settle = 0; settle <= 30; settle++)
			KUNIT_EXPECT_TRUE(test,
					  tbv_model_mcoord(nrails, true, settle, 3, 200));
}

/*
 * flaw #1: tbv_native_control_kick_matching_rail must re-arm a budget-exhausted
 * rail on an inbound peer request (HELLO/READY), mirroring net's TBIP_LOGIN
 * handler resetting login_retries. The flawed kick re-scheduled work without
 * resetting the attempt counters.
 */
static void tbv_test_native_kick_rearm(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, model_kick(HS_RETRY_BUDGET, false),
			(unsigned int)HS_RETRY_BUDGET);	/* the bug */
	KUNIT_EXPECT_EQ(test, model_kick(HS_RETRY_BUDGET, true), 0u);	/* the fix */
}

/* rx_path's peer (the requester) wins over the QP's bound peer. */
static void tbv_ack_route_peer_prefers_rx_path(struct kunit *test)
{
	struct tbv_peer requester = { .backend = TBV_BACKEND_NATIVE };
	struct tbv_peer bound = { .backend = TBV_BACKEND_NATIVE };
	struct tbv_rail rx_rail = { .peer = &requester };
	struct tbv_rail qp_rail = { .peer = &bound };
	struct tbv_path rx_path = { .rail = &rx_rail };

	/* QP bound toward `bound`, but the request came in on `requester`. */
	KUNIT_EXPECT_PTR_EQ(test, tbv_ack_route_peer(&qp_rail, &rx_path),
			    &requester);
}

/* No rx_path (timer-driven re-ACK): fall back to the QP's bound peer. */
static void tbv_ack_route_peer_falls_back_to_qp(struct kunit *test)
{
	struct tbv_peer bound = { .backend = TBV_BACKEND_NATIVE };
	struct tbv_rail qp_rail = { .peer = &bound };

	KUNIT_EXPECT_PTR_EQ(test, tbv_ack_route_peer(&qp_rail, NULL), &bound);
}

/* rx_path with no rail also falls back rather than dereferencing junk. */
static void tbv_ack_route_peer_rx_path_without_rail(struct kunit *test)
{
	struct tbv_peer bound = { .backend = TBV_BACKEND_NATIVE };
	struct tbv_rail qp_rail = { .peer = &bound };
	struct tbv_path rx_path = { .rail = NULL };

	KUNIT_EXPECT_PTR_EQ(test, tbv_ack_route_peer(&qp_rail, &rx_path),
			    &bound);
}

/* Both inputs empty -> NULL, never a crash (callers must handle NULL). */
static void tbv_ack_route_peer_all_null(struct kunit *test)
{
	KUNIT_EXPECT_NULL(test, tbv_ack_route_peer(NULL, NULL));
}

static void tbv_psn_delta_basic(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, tbv_psn_delta(5, 3), 2);
	KUNIT_EXPECT_EQ(test, tbv_psn_delta(3, 5), -2);
	KUNIT_EXPECT_EQ(test, tbv_psn_delta(7, 7), 0);
}

/* 24-bit wraparound: PSN 0 is one *after* the max PSN 0xffffff. */
static void tbv_psn_delta_wraps(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, tbv_psn_delta(0u, 0x00ffffffu), 1);
	KUNIT_EXPECT_EQ(test, tbv_psn_delta(0x00ffffffu, 0u), -1);
	/* a half-window ahead reads as the largest positive distance. */
	KUNIT_EXPECT_GT(test, tbv_psn_delta(0x007fffffu, 0u), 0);
	/* just past half-window reads as behind (duplicate territory). */
	KUNIT_EXPECT_LT(test, tbv_psn_delta(0x00800000u, 0u), 0);
}

/*
 * tbv_gid_matches_identity: maps a destination GID to the peer that advertised
 * (eui64, ipv4) in its wire-v2 HELLO. This is what lets modify_qp(RTR) bind a
 * QP to the rail that actually reaches the destination on a multi-peer node
 * (the create-time bind is a coin flip between neighbours -- the second half
 * of the appmana-020<->009 regression).
 *
 * Identity below mimics appmana-009: eno1 MAC 58:11:22:b7:76:fa ->
 * modified EUI-64 5a:11:22:ff:fe:b7:76:fa, LAN IPv4 10.2.0.15.
 */
#define TEST_EUI64 0x5a1122fffeb776faULL
#define TEST_IPV4  0x0a02000fu /* 10.2.0.15 */

static void tbv_gid_match_link_local(struct kunit *test)
{
	/* fe80::5a11:22ff:feb7:76fa */
	const u8 gid[16] = { 0xfe, 0x80, 0, 0, 0, 0, 0, 0,
			     0x5a, 0x11, 0x22, 0xff, 0xfe, 0xb7, 0x76, 0xfa };

	KUNIT_EXPECT_TRUE(test,
			  tbv_gid_matches_identity(gid, TEST_EUI64, TEST_IPV4));
}

static void tbv_gid_match_global_slaac(struct kunit *test)
{
	/* 2001:5a8:4298:3b00:5a11:22ff:feb7:76fa -- NCCL_IB_GID_INDEX=1 */
	const u8 gid[16] = { 0x20, 0x01, 0x05, 0xa8, 0x42, 0x98, 0x3b, 0x00,
			     0x5a, 0x11, 0x22, 0xff, 0xfe, 0xb7, 0x76, 0xfa };

	KUNIT_EXPECT_TRUE(test,
			  tbv_gid_matches_identity(gid, TEST_EUI64, TEST_IPV4));
}

static void tbv_gid_match_v4_mapped(struct kunit *test)
{
	/* ::ffff:10.2.0.15 -- perftest's default gid pick */
	const u8 gid[16] = { 0, 0, 0, 0, 0, 0, 0, 0,
			     0, 0, 0xff, 0xff, 10, 2, 0, 15 };

	KUNIT_EXPECT_TRUE(test,
			  tbv_gid_matches_identity(gid, TEST_EUI64, TEST_IPV4));
}

static void tbv_gid_match_wrong_peer(struct kunit *test)
{
	/* appmana-020's GID must NOT match 009's identity. */
	const u8 gid[16] = { 0xfe, 0x80, 0, 0, 0, 0, 0, 0,
			     0x5a, 0x11, 0x22, 0xff, 0xfe, 0xb7, 0x75, 0xb1 };
	const u8 v4[16] = { 0, 0, 0, 0, 0, 0, 0, 0,
			    0, 0, 0xff, 0xff, 10, 2, 0, 58 };

	KUNIT_EXPECT_FALSE(test,
			   tbv_gid_matches_identity(gid, TEST_EUI64, TEST_IPV4));
	KUNIT_EXPECT_FALSE(test,
			   tbv_gid_matches_identity(v4, TEST_EUI64, TEST_IPV4));
}

static void tbv_gid_match_zero_identity_never_matches(struct kunit *test)
{
	/* A peer with no advertised identity (zeros) matches nothing --
	 * including an all-zero GID (no false bind mid-negotiation). */
	const u8 zero_gid[16] = { 0 };
	const u8 gid[16] = { 0xfe, 0x80, 0, 0, 0, 0, 0, 0,
			     0x5a, 0x11, 0x22, 0xff, 0xfe, 0xb7, 0x76, 0xfa };

	KUNIT_EXPECT_FALSE(test, tbv_gid_matches_identity(gid, 0, 0));
	KUNIT_EXPECT_FALSE(test, tbv_gid_matches_identity(zero_gid, 0, 0));
}

static void tbv_gid_match_v4_does_not_eui_match(struct kunit *test)
{
	/* A v4-mapped GID whose low 8 bytes happen to equal the EUI-64 must
	 * still be compared as an ADDRESS, not an interface id. */
	const u8 gid[16] = { 0, 0, 0, 0, 0, 0, 0, 0,
			     0, 0, 0xff, 0xff, 0xfe, 0xb7, 0x76, 0xfa };

	KUNIT_EXPECT_FALSE(test,
			   tbv_gid_matches_identity(gid, 0x0000fffffeb776faULL,
						    TEST_IPV4));
}

/*
 * tbv_gid_subnet_match: the per-link reachability test for the honest HCA. A
 * rail reaches a peer iff the peer's GID is in the rail's local-GID subnet (the
 * per-link address udev assigns). Two ends of one link share the subnet;
 * different links do not; an off-link dgid must NOT match (it fails cleanly
 * rather than misrouting onto the rail's one real neighbour).
 */
static void tbv_subnet_match_same_64(struct kunit *test)
{
	/* fd00:99:2:7::/64 -- both ends of a link share the /64. */
	const u8 a[16] = { 0xfd,0,0x99,0,0,2,0,7, 0x5a,0x11,0x22,0xff,0xfe,0xb7,0x75,0xb1 };
	const u8 b[16] = { 0xfd,0,0x99,0,0,2,0,7, 0x12,0x21,0x1b,0x64,0xde,0x7a,0,1 };

	KUNIT_EXPECT_TRUE(test, tbv_gid_subnet_match(a, b, 64));
}

static void tbv_subnet_match_different_64(struct kunit *test)
{
	/* link 7 vs link 9 -- different /64s, not reachable. */
	const u8 a[16] = { 0xfd,0,0x99,0,0,2,0,7, 0,0,0,0,0,0,0,1 };
	const u8 c[16] = { 0xfd,0,0x99,0,0,2,0,9, 0,0,0,0,0,0,0,1 };

	KUNIT_EXPECT_FALSE(test, tbv_gid_subnet_match(a, c, 64));
}

static void tbv_subnet_match_127_link_pair(struct kunit *test)
{
	/* a /127 link: ::8 and ::9 differ only in the last bit -> same /127,
	 * but a full /128 compare must distinguish them. */
	const u8 lo[16] = { 0xfd,0,0x99,1,0,0,0,0, 0,0,0,0,0,0,0,0x08 };
	const u8 hi[16] = { 0xfd,0,0x99,1,0,0,0,0, 0,0,0,0,0,0,0,0x09 };

	KUNIT_EXPECT_TRUE(test, tbv_gid_subnet_match(lo, hi, 127));
	KUNIT_EXPECT_FALSE(test, tbv_gid_subnet_match(lo, hi, 128));
}

static void tbv_subnet_match_127_off_link(struct kunit *test)
{
	/* ::8 and ::a are in different /127s (::8-::9 vs ::a-::b). */
	const u8 a[16] = { 0xfd,0,0x99,1,0,0,0,0, 0,0,0,0,0,0,0,0x08 };
	const u8 c[16] = { 0xfd,0,0x99,1,0,0,0,0, 0,0,0,0,0,0,0,0x0a };

	KUNIT_EXPECT_FALSE(test, tbv_gid_subnet_match(a, c, 127));
}

/*
 * tbv_gid_identity_verdict: the three-state classifier behind the rebind
 * decision. The INCONCLUSIVE state exists because of the 2026-06-13 DSV4
 * boot outage: nodes HELLOed before DHCP assigned their LAN address, so
 * peers stored (eui64, ipv4=0) as a VALID identity. Treating that as a
 * plain non-match made "every peer validly not matching" true and
 * modify_qp(RTR) hard-failed -ENETUNREACH against the node's own cabled
 * neighbour. An identity that cannot adjudicate the dgid's address family
 * must abstain, never vote.
 */
static void tbv_verdict_v4_dgid_zero_ipv4_is_inconclusive(struct kunit *test)
{
	/* ::ffff:10.2.0.4 against identity (eui64, ipv4=0): the DSV4 bug. */
	const u8 gid[16] = { 0, 0, 0, 0, 0, 0, 0, 0,
			     0, 0, 0xff, 0xff, 10, 2, 0, 4 };

	KUNIT_EXPECT_EQ(test, TBV_IDENTITY_INCONCLUSIVE,
			tbv_gid_identity_verdict(gid, true, TEST_EUI64, 0));
}

static void tbv_verdict_v4_dgid_match_and_mismatch(struct kunit *test)
{
	const u8 gid[16] = { 0, 0, 0, 0, 0, 0, 0, 0,
			     0, 0, 0xff, 0xff, 10, 2, 0, 15 };

	KUNIT_EXPECT_EQ(test, TBV_IDENTITY_MATCH,
			tbv_gid_identity_verdict(gid, true, TEST_EUI64,
						 TEST_IPV4));
	KUNIT_EXPECT_EQ(test, TBV_IDENTITY_NO_MATCH,
			tbv_gid_identity_verdict(gid, true, TEST_EUI64,
						 0x0a020039u /* 10.2.0.57 */));
}

static void tbv_verdict_eui_dgid_zero_eui_is_inconclusive(struct kunit *test)
{
	/* fe80:: dgid against identity (eui64=0, ipv4 set): can't compare. */
	const u8 gid[16] = { 0xfe, 0x80, 0, 0, 0, 0, 0, 0,
			     0x5a, 0x11, 0x22, 0xff, 0xfe, 0xb7, 0x76, 0xfa };

	KUNIT_EXPECT_EQ(test, TBV_IDENTITY_INCONCLUSIVE,
			tbv_gid_identity_verdict(gid, true, 0, TEST_IPV4));
}

static void tbv_verdict_invalid_identity_is_inconclusive(struct kunit *test)
{
	const u8 gid[16] = { 0, 0, 0, 0, 0, 0, 0, 0,
			     0, 0, 0xff, 0xff, 10, 2, 0, 15 };

	KUNIT_EXPECT_EQ(test, TBV_IDENTITY_INCONCLUSIVE,
			tbv_gid_identity_verdict(gid, false, TEST_EUI64,
						 TEST_IPV4));
}

static void tbv_verdict_match_wrapper_contract_unchanged(struct kunit *test)
{
	/* tbv_gid_matches_identity keeps its boolean contract: only a
	 * conclusive MATCH returns true; INCONCLUSIVE stays false. */
	const u8 gid[16] = { 0, 0, 0, 0, 0, 0, 0, 0,
			     0, 0, 0xff, 0xff, 10, 2, 0, 15 };

	KUNIT_EXPECT_TRUE(test,
			  tbv_gid_matches_identity(gid, TEST_EUI64, TEST_IPV4));
	KUNIT_EXPECT_FALSE(test,
			   tbv_gid_matches_identity(gid, TEST_EUI64, 0));
}

/*
 * Re-HELLO supersede (the XDomain re-negotiation hang): an inbound HELLO on an
 * already-established native rail means the peer soft-reloaded (no link edge, no
 * remove event), so its old rings are gone. tbv_native_control_apply_remote()
 * must supersede the latched handshake (the shared cross-driver contract,
 * mirroring thunderbolt_net's LOGOUT reset) so the rail stops reporting
 * data-ready into a dead tunnel and re-confirms. Exercises the REAL
 * tbv_rail_data_ready() and tb_xdomain_handshake_supersede(); the model step
 * matches apply_remote's inbound-HELLO path.
 */
static void tbv_test_native_rehello_supersede(struct kunit *test)
{
	struct tbv_peer peer = { .backend = TBV_BACKEND_NATIVE };
	struct tbv_rail rail;

	memset(&rail, 0, sizeof(rail));
	rail.peer = &peer;
	rail.path.state = TBV_PATH_TUNNEL_ENABLED;
	rail.native_hs.request_sent = true;
	rail.native_hs.peer_seen = true;
	rail.native_hs.established = true;
	rail.native_negotiated = true;
	rail.remote_transmit_path = 5;
	KUNIT_EXPECT_TRUE(test, tbv_rail_data_ready(&rail));

	/* peer restarts and re-HELLOs with new hops (apply_remote inbound path) */
	rail.native_negotiated = true;
	rail.remote_transmit_path = 9;
	tb_xdomain_handshake_supersede(&rail.native_hs);

	/* must no longer be data-ready until it re-handshakes */
	KUNIT_EXPECT_FALSE(test, tbv_rail_data_ready(&rail));
}

/*
 * Changed-hops rehop: when the re-HELLO carries a different out-hop than the
 * live tunnel was enabled with, supersede alone is not enough -- the tunnel must
 * be disabled (-> RING_STARTED) and re-enabled with the new hop, else the rail
 * re-confirms data-ready into a dead hop (enable_tunnel rejects the change with
 * -EBUSY, path.c:1380). Models the work's rehop path + tbv_path_*_tunnel state
 * transitions.
 */
static void tbv_test_native_rehello_changed_hops(struct kunit *test)
{
	struct tbv_peer peer = { .backend = TBV_BACKEND_NATIVE };
	struct tbv_rail rail;
	bool rehop;

	memset(&rail, 0, sizeof(rail));
	rail.peer = &peer;
	rail.path.state = TBV_PATH_TUNNEL_ENABLED;
	rail.path.remote_transmit_path = 5;	/* tunnel enabled with peer hop 5 */
	rail.remote_transmit_path = 5;
	rail.native_negotiated = true;
	rail.native_hs.request_sent = true;
	rail.native_hs.peer_seen = true;

	/* inbound HELLO with a new hop (9): supersede + flag rehop */
	rail.remote_transmit_path = 9;
	rehop = rail.path.state == TBV_PATH_TUNNEL_ENABLED &&
		rail.path.remote_transmit_path != rail.remote_transmit_path;
	tb_xdomain_handshake_supersede(&rail.native_hs);
	KUNIT_EXPECT_TRUE(test, rehop);

	/* work: disable stale tunnel, re-enable with new hop, re-handshake */
	if (rehop) {
		rail.path.remote_transmit_path = -1;
		rail.path.state = TBV_PATH_RING_STARTED;
	}
	rail.path.remote_transmit_path = rail.remote_transmit_path;
	rail.path.state = TBV_PATH_TUNNEL_ENABLED;
	rail.native_hs.request_sent = true;
	rail.native_hs.peer_seen = true;

	KUNIT_EXPECT_TRUE(test, tbv_rail_data_ready(&rail));
	KUNIT_EXPECT_EQ(test, rail.path.remote_transmit_path,
			rail.remote_transmit_path);
}

/*
 * ============================================================================
 * Cohesive negotiation macros (TB_XNEG_*) -- the installable negotiation both
 * thunderbolt_ibverbs and thunderbolt_net embed. These cover EVERYTHING the
 * macros fix, so the same KUnit guards both drivers' install:
 *   - tb_xdomain_generation_stale (the gate)
 *   - tb_xdomain_services_added/removed/changed (the service-set predicates)
 *   - TB_XNEG_ENUMERATE/RECONNECT (the forced-rescan that binds a newly-
 *     advertised service after a soft reload whose notification was lost)
 * ============================================================================
 */
struct xneg_link_peer { TB_XNEG_STATE; };

/* a driver poll: re-read iff notified OR a reconnect forced a rescan */
static void xneg_poll(struct xneg_link_peer *self,
		      const struct xneg_link_peer *peer, bool notified)
{
	if (!(notified || self->xneg_force_rescan))
		return;
	TB_XNEG_ENUMERATE(self, peer->xneg_local_gen, peer->xneg_local_svcs,
			  /*_bind*/ (void)svc, /*_unbind*/ (void)svc);
}

/* soft reload flipping tbnet_identity=stock; fix forces the rescan, notify lost */
static void xneg_reload_add_tbnet(struct xneg_link_peer *self, bool fix)
{
	if (fix)
		TB_XNEG_RECONNECT(self);
	else
		tb_xdomain_handshake_reset(&self->xneg_hs);
	TB_XNEG_ADVERTISE(self, TB_XSVC_NATIVE | TB_XSVC_TBNET);
}

/* coordinated reload adding tbnet; returns whether BOTH peers bound tbnet */
static bool xneg_tbnet_binds_after_reload(bool fix)
{
	struct xneg_link_peer a, b;

	TB_XNEG_INIT(&a, TB_XSVC_NATIVE);
	TB_XNEG_INIT(&b, TB_XSVC_NATIVE);
	xneg_poll(&a, &b, true);
	xneg_poll(&b, &a, true);
	xneg_reload_add_tbnet(&a, fix);
	xneg_reload_add_tbnet(&b, fix);
	for (int i = 0; i < 10; i++) {		/* notifications never arrive */
		xneg_poll(&a, &b, false);
		xneg_poll(&b, &a, false);
	}
	return TB_XNEG_BOUND(&a, TB_XSVC_TBNET) && TB_XNEG_BOUND(&b, TB_XSVC_TBNET);
}

/* the gate: drop ONLY an exact-duplicate re-read; a lower gen means the peer
 * rebooted (random reseed) and MUST be accepted; a 0 cache forces accept */
static void tbv_test_xneg_generation_gate(struct kunit *test)
{
	KUNIT_EXPECT_FALSE(test, tb_xdomain_generation_stale(false, 1, 7)); /* first read */
	KUNIT_EXPECT_TRUE(test, tb_xdomain_generation_stale(true, 5, 5));   /* equal: duplicate, stale */
	KUNIT_EXPECT_FALSE(test, tb_xdomain_generation_stale(true, 4, 5));  /* lower: peer rebooted, accept */
	KUNIT_EXPECT_FALSE(test, tb_xdomain_generation_stale(true, 6, 5));  /* newer: accept */
	KUNIT_EXPECT_FALSE(test, tb_xdomain_generation_stale(true, 1, 0));  /* forced re-read */
}

/* the service-set predicates that drive bind/unbind */
static void tbv_test_xneg_service_set_predicates(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, tb_xdomain_services_added(TB_XSVC_NATIVE,
			TB_XSVC_NATIVE | TB_XSVC_TBNET), (u32)TB_XSVC_TBNET);
	KUNIT_EXPECT_EQ(test, tb_xdomain_services_removed(
			TB_XSVC_NATIVE | TB_XSVC_TBNET, TB_XSVC_NATIVE),
			(u32)TB_XSVC_TBNET);
	KUNIT_EXPECT_TRUE(test, tb_xdomain_service_set_changed(TB_XSVC_NATIVE,
			TB_XSVC_NATIVE | TB_XSVC_TBNET));
	KUNIT_EXPECT_FALSE(test, tb_xdomain_service_set_changed(TB_XSVC_NATIVE,
			TB_XSVC_NATIVE));
}

/*
 * THE renegotiation gap: after a coordinated soft reload that advertises tbnet,
 * the peer binds thunderbolt_net ONLY if the reconnect forces a directory
 * rescan. Trusting the (lost) best-effort notification strands it -- the
 * "thunderbolt_net loads then unloads, no tbX iface" symptom.
 */
static void tbv_test_xneg_tbnet_bind_renegotiation(struct kunit *test)
{
	KUNIT_EXPECT_FALSE(test, xneg_tbnet_binds_after_reload(false)); /* the bug */
	KUNIT_EXPECT_TRUE(test, xneg_tbnet_binds_after_reload(true));   /* the fix */
}

static struct kunit_case tbv_ack_routing_test_cases[] = {
	KUNIT_CASE(tbv_test_xneg_generation_gate),
	KUNIT_CASE(tbv_test_xneg_service_set_predicates),
	KUNIT_CASE(tbv_test_xneg_tbnet_bind_renegotiation),
	KUNIT_CASE(tbv_test_native_handshake_hang),
	KUNIT_CASE(tbv_test_native_rehello_supersede),
	KUNIT_CASE(tbv_test_native_rehello_changed_hops),
	KUNIT_CASE(tbv_test_native_handshake_multirail),
	KUNIT_CASE(tbv_test_native_kick_rearm),
	KUNIT_CASE(tbv_ack_route_peer_prefers_rx_path),
	KUNIT_CASE(tbv_ack_route_peer_falls_back_to_qp),
	KUNIT_CASE(tbv_ack_route_peer_rx_path_without_rail),
	KUNIT_CASE(tbv_ack_route_peer_all_null),
	KUNIT_CASE(tbv_psn_delta_basic),
	KUNIT_CASE(tbv_psn_delta_wraps),
	KUNIT_CASE(tbv_gid_match_link_local),
	KUNIT_CASE(tbv_gid_match_global_slaac),
	KUNIT_CASE(tbv_gid_match_v4_mapped),
	KUNIT_CASE(tbv_gid_match_wrong_peer),
	KUNIT_CASE(tbv_gid_match_zero_identity_never_matches),
	KUNIT_CASE(tbv_gid_match_v4_does_not_eui_match),
	KUNIT_CASE(tbv_subnet_match_same_64),
	KUNIT_CASE(tbv_subnet_match_different_64),
	KUNIT_CASE(tbv_subnet_match_127_link_pair),
	KUNIT_CASE(tbv_subnet_match_127_off_link),
	KUNIT_CASE(tbv_verdict_v4_dgid_zero_ipv4_is_inconclusive),
	KUNIT_CASE(tbv_verdict_v4_dgid_match_and_mismatch),
	KUNIT_CASE(tbv_verdict_eui_dgid_zero_eui_is_inconclusive),
	KUNIT_CASE(tbv_verdict_invalid_identity_is_inconclusive),
	KUNIT_CASE(tbv_verdict_match_wrapper_contract_unchanged),
	{}
};

static struct kunit_suite tbv_ack_routing_test_suite = {
	.name = "thunderbolt_ibverbs_ack_routing",
	.test_cases = tbv_ack_routing_test_cases,
};
kunit_test_suite(tbv_ack_routing_test_suite);
