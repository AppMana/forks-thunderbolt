// SPDX-License-Identifier: GPL-2.0
/*
 * Userspace mirror of kernel/tests/ack_routing_test.c (KUnit).
 *
 * The fleet's Ubuntu generic kernels do not set CONFIG_KUNIT, so the kunit
 * suite cannot run on the nodes. This harness duplicates the two PURE
 * functions under test verbatim and runs the exact same cases, so the
 * contract can be verified on any host with `cc`. If you change
 * tbv_ack_route_peer() or tbv_psn_delta() in kernel/ibdev.c, update BOTH this
 * mirror and the kunit suite (they are deliberately tiny so drift is obvious).
 *
 * build+run: cc -O2 -Wall -o /tmp/ack_test ack_routing_userspace.c && /tmp/ack_test
 */
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>

typedef int32_t s32;
typedef uint32_t u32;

/* --- minimal struct shapes (only the fields the logic touches) --- */
enum tbv_backend_type { TBV_BACKEND_APPLE, TBV_BACKEND_NATIVE };
struct tbv_peer { enum tbv_backend_type backend; };
struct tbv_rail { struct tbv_peer *peer; };
struct tbv_path { struct tbv_rail *rail; };

/* --- MIRROR of tbv_ack_route_peer() in kernel/ibdev.c --- */
static struct tbv_peer *tbv_ack_route_peer(struct tbv_rail *qp_rail,
					   struct tbv_path *rx_path)
{
	if (rx_path && rx_path->rail && rx_path->rail->peer)
		return rx_path->rail->peer;
	return qp_rail ? qp_rail->peer : NULL;
}

/* --- MIRROR of tbv_psn_delta() in kernel/ibdev.c --- */
#define TBV_PSN_MASK 0x00ffffffu
static s32 tbv_psn_delta(u32 a, u32 b)
{
	u32 delta = (a - b) & TBV_PSN_MASK;

	if (delta & 0x00800000u)
		return (s32)(delta | 0xff000000u);
	return (s32)delta;
}

/* --- MIRROR of tbv_gid_matches_identity() in kernel/ibdev.c --- */
#include <string.h>
typedef uint64_t u64;
typedef uint8_t u8;
enum tbv_identity_verdict {
	TBV_IDENTITY_NO_MATCH = 0,
	TBV_IDENTITY_MATCH,
	TBV_IDENTITY_INCONCLUSIVE,
};

static enum tbv_identity_verdict
tbv_gid_identity_verdict(const u8 gid[16], int identity_valid,
			 u64 eui64, u32 ipv4)
{
	static const u8 v4_prefix[12] = {
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff,
	};

	if (!identity_valid)
		return TBV_IDENTITY_INCONCLUSIVE;

	if (!memcmp(gid, v4_prefix, sizeof(v4_prefix))) {
		u32 addr = ((u32)gid[12] << 24) | ((u32)gid[13] << 16) |
			   ((u32)gid[14] << 8) | (u32)gid[15];

		if (!ipv4)
			return TBV_IDENTITY_INCONCLUSIVE;
		return addr == ipv4 ? TBV_IDENTITY_MATCH :
				      TBV_IDENTITY_NO_MATCH;
	}

	if (eui64) {
		u64 iid = ((u64)gid[8] << 56) | ((u64)gid[9] << 48) |
			  ((u64)gid[10] << 40) | ((u64)gid[11] << 32) |
			  ((u64)gid[12] << 24) | ((u64)gid[13] << 16) |
			  ((u64)gid[14] << 8) | (u64)gid[15];

		return iid == eui64 ? TBV_IDENTITY_MATCH :
				      TBV_IDENTITY_NO_MATCH;
	}

	return TBV_IDENTITY_INCONCLUSIVE;
}

static int tbv_gid_matches_identity(const u8 gid[16], u64 eui64, u32 ipv4)
{
	return tbv_gid_identity_verdict(gid, 1, eui64, ipv4) ==
	       TBV_IDENTITY_MATCH;
}

static int failures;
#define EXPECT(cond, name) do { \
	if (cond) { printf("ok   %s\n", name); } \
	else { printf("FAIL %s\n", name); failures++; } \
} while (0)

int main(void)
{
	/* ACK routes to the requester (rx_path's peer), not the QP's bound peer.
	 * This is the appmana-020<->009 regression: a mid-chain responder QP
	 * round-robin-bound toward the wrong neighbour sent every ACK out the
	 * wrong rail -> requester saw IBV_WC_RETRY_EXC_ERR though writes
	 * arrived. */
	{
		struct tbv_peer requester = { TBV_BACKEND_NATIVE };
		struct tbv_peer bound = { TBV_BACKEND_NATIVE };
		struct tbv_rail rx_rail = { &requester };
		struct tbv_rail qp_rail = { &bound };
		struct tbv_path rx_path = { &rx_rail };

		EXPECT(tbv_ack_route_peer(&qp_rail, &rx_path) == &requester,
		       "ack_route_peer prefers rx_path peer (requester)");
		EXPECT(tbv_ack_route_peer(&qp_rail, NULL) == &bound,
		       "ack_route_peer falls back to QP bound peer (re-ACK timer)");
	}
	{
		struct tbv_peer bound = { TBV_BACKEND_NATIVE };
		struct tbv_rail qp_rail = { &bound };
		struct tbv_path rx_path_no_rail = { NULL };

		EXPECT(tbv_ack_route_peer(&qp_rail, &rx_path_no_rail) == &bound,
		       "ack_route_peer rx_path without rail falls back");
		EXPECT(tbv_ack_route_peer(NULL, NULL) == NULL,
		       "ack_route_peer all-NULL returns NULL (no crash)");
	}

	/* PSN arithmetic: signed 24-bit distance with wraparound. */
	EXPECT(tbv_psn_delta(5, 3) == 2, "psn_delta basic forward");
	EXPECT(tbv_psn_delta(3, 5) == -2, "psn_delta basic backward");
	EXPECT(tbv_psn_delta(7, 7) == 0, "psn_delta equal");
	EXPECT(tbv_psn_delta(0u, 0x00ffffffu) == 1, "psn_delta wraps forward");
	EXPECT(tbv_psn_delta(0x00ffffffu, 0u) == -1, "psn_delta wraps backward");
	EXPECT(tbv_psn_delta(0x007fffffu, 0u) > 0, "psn_delta half-window ahead");
	EXPECT(tbv_psn_delta(0x00800000u, 0u) < 0, "psn_delta past half-window behind");

	/* GID -> peer identity matching (RTR rebind, wire v2). Identity mimics
	 * appmana-009: MAC 58:11:22:b7:76:fa -> EUI-64 5a1122fffeb776fa,
	 * 10.2.0.15. */
	{
		const u64 eui = 0x5a1122fffeb776faULL;
		const u32 v4 = 0x0a02000fu;
		const u8 ll[16]   = { 0xfe, 0x80, 0, 0, 0, 0, 0, 0,
				      0x5a, 0x11, 0x22, 0xff, 0xfe, 0xb7, 0x76, 0xfa };
		const u8 slaac[16] = { 0x20, 0x01, 0x05, 0xa8, 0x42, 0x98, 0x3b, 0x00,
				       0x5a, 0x11, 0x22, 0xff, 0xfe, 0xb7, 0x76, 0xfa };
		const u8 v4m[16]  = { 0, 0, 0, 0, 0, 0, 0, 0,
				      0, 0, 0xff, 0xff, 10, 2, 0, 15 };
		const u8 other[16] = { 0xfe, 0x80, 0, 0, 0, 0, 0, 0,
				       0x5a, 0x11, 0x22, 0xff, 0xfe, 0xb7, 0x75, 0xb1 };
		const u8 zero[16] = { 0 };
		const u8 v4_eui_collision[16] = { 0, 0, 0, 0, 0, 0, 0, 0,
						  0, 0, 0xff, 0xff, 0xfe, 0xb7, 0x76, 0xfa };

		EXPECT(tbv_gid_matches_identity(ll, eui, v4),
		       "gid match: link-local EUI-64");
		EXPECT(tbv_gid_matches_identity(slaac, eui, v4),
		       "gid match: global SLAAC EUI-64 (NCCL gid index 1)");
		EXPECT(tbv_gid_matches_identity(v4m, eui, v4),
		       "gid match: IPv4-mapped (perftest default)");
		EXPECT(!tbv_gid_matches_identity(other, eui, v4),
		       "gid match: wrong peer's GID rejected");
		EXPECT(!tbv_gid_matches_identity(ll, 0, 0),
		       "gid match: zero identity never matches");
		EXPECT(!tbv_gid_matches_identity(zero, 0, 0),
		       "gid match: zero GID vs zero identity rejected");
		EXPECT(!tbv_gid_matches_identity(v4_eui_collision,
						 0x0000fffffeb776faULL, v4),
		       "gid match: v4-mapped compared as address, not EUI");

		/* Verdict classifier: the 2026-06-13 DSV4 boot bug. An
		 * identity stored from a pre-DHCP HELLO (eui64, ipv4=0) must
		 * ABSTAIN on a v4-mapped dgid, never vote "not me" -- that
		 * vote is what turned modify_qp(RTR) into -ENETUNREACH
		 * against the node's own cabled neighbour. */
		EXPECT(tbv_gid_identity_verdict(v4m, 1, eui, 0) ==
		       TBV_IDENTITY_INCONCLUSIVE,
		       "verdict: v4 dgid vs ipv4=0 identity abstains (DSV4 boot bug)");
		EXPECT(tbv_gid_identity_verdict(v4m, 1, eui, v4) ==
		       TBV_IDENTITY_MATCH,
		       "verdict: v4 dgid conclusive match");
		EXPECT(tbv_gid_identity_verdict(v4m, 1, eui, 0x0a020039u) ==
		       TBV_IDENTITY_NO_MATCH,
		       "verdict: v4 dgid conclusive mismatch");
		EXPECT(tbv_gid_identity_verdict(ll, 1, 0, v4) ==
		       TBV_IDENTITY_INCONCLUSIVE,
		       "verdict: EUI dgid vs eui64=0 identity abstains");
		EXPECT(tbv_gid_identity_verdict(v4m, 0, eui, v4) ==
		       TBV_IDENTITY_INCONCLUSIVE,
		       "verdict: invalid identity abstains");
	}

	printf("%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
	return failures ? 1 : 0;
}
