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
 * Build: included in the module only when CONFIG_KUNIT is set
 * (see kernel/Makefile). Run on a CONFIG_KUNIT kernel, e.g. via kunit.py, or
 * mirror the same cases in userspace (kernel/tests/ack_routing_userspace.c).
 */
#include <kunit/test.h>
#include "../tbv.h"

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

static struct kunit_case tbv_ack_routing_test_cases[] = {
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
