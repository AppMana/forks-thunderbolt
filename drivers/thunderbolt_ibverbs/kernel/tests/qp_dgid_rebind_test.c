// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit: a native RC/UC QP must send on the rail of the peer that owns its
 * destination GID, not the create-time (coin-flip) home rail. This is the
 * data-path half of the multi-peer routing bug -- the "ACKs misrouted" hang
 * the 6-rank all_reduce hit once create_qp stopped returning -ENOTCONN.
 *
 * The QP is rail-bound at create_qp BEFORE the destination is known; on a
 * mid-chain node cabled to two neighbours that binds to the ib_device's home
 * peer, which need NOT be the peer NCCL connects this QP to. tbv_create_qp then
 * sends every frame via tqp->rail->path
 * (tbv_select_native_data_path_for_qp_locked -- there is NO per-send dgid
 * re-selection), so a QP bound to the wrong neighbour egresses out the wrong
 * rail: a third node never matches the QPN, drops the frame with no NAK, and
 * the sender dies IBV_WC_RETRY_EXC_ERR while the intended responder never saw
 * the write. The destination GID (known at modify_qp(RTR)) identifies the true
 * peer via the identity it advertised in its wire-v2 HELLO
 * (remote_roce_eui64/ipv4), so the QP must be REBOUND there -- the peer
 * identity is stored expressly for this (tbv.h) but the rebind was never wired
 * until now (tbv_qp_rebind_to_dgid).
 *
 * Route consistency is the invariant: after RTR the QP's SEND rail == the
 * dgid's peer == the peer whose ACKs come back (rx_path). This calls the REAL
 * predicate tbv_gid_identity_verdict to resolve dgid->peer; TBV_QP_DGID_REBIND
 * is the lockstep lever (built =0 reproduces the misroute). Run via
 * tools/run-kunit.sh.
 */
#include <kunit/test.h>
#include <linux/types.h>
#include <linux/string.h>
#include "../tbv.h"

/* Lockstep with tbv_qp_rebind_to_dgid(): 1 = RTR rebinds the QP to the dgid's
 * peer; 0 = pre-fix (QP keeps the create-time home rail -> misroute). */
#ifndef TBV_QP_DGID_REBIND
#define TBV_QP_DGID_REBIND 1
#endif

struct dr_peer {
	u64 eui64;	/* the peer's advertised RoCE modified-EUI-64 */
	bool valid;	/* remote_identity_valid (false until HELLO/DHCP) */
};

/* A link-local RoCE GID carrying @eui64 as its interface id (bytes 8..15, BE) --
 * the exact shape tbv_gid_identity_verdict reads for the non-v4-mapped case. */
static void dr_gid_from_eui64(u8 gid[16], u64 eui64)
{
	int i;

	memset(gid, 0, 16);
	gid[0] = 0xfe;
	gid[1] = 0x80;
	for (i = 0; i < 8; i++)
		gid[8 + i] = (u8)(eui64 >> (56 - 8 * i));
}

/* Model of tbv_peer_for_dgid_locked() using the REAL verdict: the sole native
 * peer that MATCHES, or 0 for unknown / inconclusive / ambiguous. */
static u64 dr_resolve_dgid(const struct dr_peer *peers, int n, const u8 dgid[16])
{
	u64 match = 0;
	int matches = 0;
	int i;

	for (i = 0; i < n; i++) {
		if (!peers[i].valid)
			continue;
		if (tbv_gid_identity_verdict(dgid, true, peers[i].eui64, 0) !=
		    TBV_IDENTITY_MATCH)
			continue;
		match = peers[i].eui64;
		matches++;
	}
	return matches == 1 ? match : 0;
}

/* The QP's SEND-rail peer after RTR: create-time home, rebound to the dgid's
 * peer when the fix is in and the dgid resolves. */
static u64 dr_send_peer_after_rtr(u64 home_eui64, const struct dr_peer *peers,
				  int n, const u8 dgid[16])
{
	u64 dest = dr_resolve_dgid(peers, n, dgid);

	if (TBV_QP_DGID_REBIND && dest)
		return dest;
	return home_eui64;
}

/* THE MISROUTE: QP created on the home peer (H) but connected to the other
 * neighbour (D). Send rail must follow the dgid to D and match the ACK return
 * peer (also D). RED with TBV_QP_DGID_REBIND=0 (send stays H != D). */
static void tbv_qp_send_follows_dgid_on_multipeer(struct kunit *test)
{
	const u64 H = 0x0200544257524253ULL;	/* home neighbour identity */
	const u64 D = 0x02005442aa55aa55ULL;	/* the connected neighbour */
	struct dr_peer peers[2] = { { H, true }, { D, true } };
	u8 dgid[16];
	u64 send_peer, ack_peer;

	dr_gid_from_eui64(dgid, D);		/* NCCL connects this QP to D */

	send_peer = dr_send_peer_after_rtr(H, peers, 2, dgid);
	ack_peer = dr_resolve_dgid(peers, 2, dgid);	/* ACKs come back from D */

	KUNIT_EXPECT_EQ(test, send_peer, D);		/* not the home coin-flip */
	KUNIT_EXPECT_EQ(test, send_peer, ack_peer);	/* route consistency */
}

/* When the QP was already bound to the right neighbour, RTR is a no-op. */
static void tbv_qp_send_stable_when_home_is_dgid(struct kunit *test)
{
	const u64 H = 0x0200544257524253ULL;
	const u64 D = 0x02005442aa55aa55ULL;
	struct dr_peer peers[2] = { { H, true }, { D, true } };
	u8 dgid[16];

	dr_gid_from_eui64(dgid, H);		/* connected to the home peer */
	KUNIT_EXPECT_EQ(test, dr_send_peer_after_rtr(H, peers, 2, dgid), H);
}

/* Single-peer node: only one neighbour, so the create-time binding is always
 * right and stays put. */
static void tbv_qp_send_single_peer(struct kunit *test)
{
	const u64 H = 0x0200544257524253ULL;
	struct dr_peer peers[1] = { { H, true } };
	u8 dgid[16];

	dr_gid_from_eui64(dgid, H);
	KUNIT_EXPECT_EQ(test, dr_send_peer_after_rtr(H, peers, 1, dgid), H);
}

/* An unknown or inconclusive dgid must NOT move the binding (never fail RTR):
 * the fix leaves the create-time rail untouched, degrading to prior behaviour
 * rather than stranding the QP. */
static void tbv_qp_send_keeps_binding_on_unresolved_dgid(struct kunit *test)
{
	const u64 H = 0x0200544257524253ULL;
	const u64 D = 0x02005442aa55aa55ULL;
	const u64 UNKNOWN = 0x02005442deadbeefULL;
	/* D present but its HELLO/DHCP identity has not landed -> inconclusive. */
	struct dr_peer peers[2] = { { H, true }, { D, false } };
	u8 dgid[16];

	dr_gid_from_eui64(dgid, UNKNOWN);
	KUNIT_EXPECT_EQ(test, dr_send_peer_after_rtr(H, peers, 2, dgid), H);

	dr_gid_from_eui64(dgid, D);	/* D itself, but identity not valid yet */
	KUNIT_EXPECT_EQ(test, dr_send_peer_after_rtr(H, peers, 2, dgid), H);
}

/*
 * Rail-ULA dgid resolution (probe-095 lockup, 2026-07-19): NCCL's rail
 * routing targets the per-link ULA GIDs (fd<link-/64>::1/::2 assigned by
 * tbv-rdma-addr), whose role-based interface ids can NEVER match a peer's
 * advertised eui64/ipv4 identity -- tbv_peer_for_dgid_locked returns NULL,
 * the QP keeps its create-time coin flip, and on a mid-chain node the
 * frames egress the wrong neighbour (appmana-027 22:51 "dgid unresolved on
 * multi-peer node", pipeline dead at the 018->027 hop at 507k tokens).
 *
 * The deterministic resolver needs no peer cooperation at all: the /64 of a
 * per-link ULA is shared by exactly the two cabled ends, and OUR OWN rail
 * ibdev's GID table holds the local half. dgid /64 == local rail ULA /64
 * identifies THAT rail's peer. fe80::/64 is identical on every rail and
 * must never prefix-match; a /64 claimed by two local rails is ambiguous
 * and must not move the binding.
 */

/* Lockstep with tbv_peer_for_ula_dgid_locked(): 1 = an identity-unresolved
 * ULA dgid falls back to the local rail whose assigned ULA shares its /64;
 * 0 = pre-fix (keep the create-time coin flip -> probe-095 misroute). */
#ifndef TBV_QP_DGID_ULA_FALLBACK
#define TBV_QP_DGID_ULA_FALLBACK 1
#endif

struct dr_rail {
	u64 peer_eui64;	/* which neighbour this rail is cabled to */
	u8 local_ula[16];	/* the fd<link>::N our side carries (0 = none) */
};

/* fd<link-/64>::<iid> -- the exact shape tbv-rdma-addr assigns per link. */
static void dr_gid_rail_ula(u8 gid[16], u64 link64, u8 iid)
{
	int i;

	memset(gid, 0, 16);
	for (i = 0; i < 8; i++)
		gid[i] = (u8)(link64 >> (56 - 8 * i));
	gid[15] = iid;
}

/* Test-local model of the /64 fallback (replaced by the real helper once the
 * driver grows one): ULA-only, exact /64, unique across local rails. */
static u64 dr_resolve_dgid_by_local_rail_ula(const struct dr_rail *rails,
					     int n, const u8 dgid[16])
{
	u64 match = 0;
	int matches = 0;
	int i;

	if ((dgid[0] & 0xfe) != 0xfc)	/* not fc00::/7 (ULA) -> no fallback */
		return 0;
	for (i = 0; i < n; i++) {
		if ((rails[i].local_ula[0] & 0xfe) != 0xfc)
			continue;
		if (memcmp(rails[i].local_ula, dgid, 8) != 0)
			continue;
		if (matches && match != rails[i].peer_eui64)
			return 0;	/* two local rails claim the /64 */
		match = rails[i].peer_eui64;
		matches++;
	}
	return match;
}

/* Resolution chain as the driver runs it at RTR: advertised identity first,
 * then (with the fix) the local-rail ULA /64 fallback. */
static u64 dr_send_peer_after_rtr_v2(u64 home_eui64,
				     const struct dr_peer *peers, int np,
				     const struct dr_rail *rails, int nr,
				     const u8 dgid[16])
{
	u64 dest = dr_resolve_dgid(peers, np, dgid);

	if (!dest && TBV_QP_DGID_ULA_FALLBACK)
		dest = dr_resolve_dgid_by_local_rail_ula(rails, nr, dgid);
	if (TBV_QP_DGID_REBIND && dest)
		return dest;
	return home_eui64;
}

/* THE PROBE-095 MISROUTE: dgid is the neighbour's rail ULA. Identities are
 * even valid -- they just can't match a role-based ::1 interface id. The QP
 * must follow the /64 to the cabled neighbour, not keep the coin flip. */
static void tbv_qp_send_follows_rail_ula_dgid_on_multipeer(struct kunit *test)
{
	const u64 H = 0x0200544257524253ULL;
	const u64 D = 0x02005442aa55aa55ULL;
	const u64 LINK_HD = 0xfd727599a7b95cb3ULL;	/* the 018<->027 /64 */
	const u64 LINK_HX = 0xfd59f99cc6d6560eULL;	/* the other rail's /64 */
	struct dr_peer peers[2] = { { H, true }, { D, true } };
	struct dr_rail rails[2];
	u8 dgid[16];

	rails[0].peer_eui64 = H;
	dr_gid_rail_ula(rails[0].local_ula, LINK_HX, 2);
	rails[1].peer_eui64 = D;
	dr_gid_rail_ula(rails[1].local_ula, LINK_HD, 2);

	dr_gid_rail_ula(dgid, LINK_HD, 1);	/* the peer's ::1 on D's link */
	KUNIT_EXPECT_EQ(test,
			dr_send_peer_after_rtr_v2(H, peers, 2, rails, 2, dgid),
			D);
}

/* Same, with the peer's HELLO identity never landed (raced session): the
 * fallback must not depend on the peer having advertised anything. */
static void tbv_qp_send_follows_rail_ula_when_identity_invalid(struct kunit *test)
{
	const u64 H = 0x0200544257524253ULL;
	const u64 D = 0x02005442aa55aa55ULL;
	const u64 LINK_HD = 0xfd727599a7b95cb3ULL;
	struct dr_peer peers[2] = { { H, true }, { D, false } };
	struct dr_rail rails[1];
	u8 dgid[16];

	rails[0].peer_eui64 = D;
	dr_gid_rail_ula(rails[0].local_ula, LINK_HD, 2);

	dr_gid_rail_ula(dgid, LINK_HD, 1);
	KUNIT_EXPECT_EQ(test,
			dr_send_peer_after_rtr_v2(H, peers, 2, rails, 1, dgid),
			D);
}

/* fe80::/64 is the same on EVERY rail -- a link-local dgid that the identity
 * path cannot resolve must never be prefix-matched (ambiguous by
 * construction). Binding stays put. */
static void tbv_qp_ula_fallback_ignores_linklocal(struct kunit *test)
{
	const u64 H = 0x0200544257524253ULL;
	const u64 D = 0x02005442aa55aa55ULL;
	const u64 UNKNOWN = 0x02005442deadbeefULL;
	struct dr_peer peers[2] = { { H, true }, { D, true } };
	struct dr_rail rails[1];
	u8 dgid[16];

	rails[0].peer_eui64 = D;
	dr_gid_from_eui64(rails[0].local_ula, D);	/* fe80-shaped local gid */

	dr_gid_from_eui64(dgid, UNKNOWN);
	KUNIT_EXPECT_EQ(test,
			dr_send_peer_after_rtr_v2(H, peers, 2, rails, 1, dgid),
			H);
}

/* A /64 claimed by two local rails (misconfiguration) is ambiguous: the
 * fallback must not move the binding. */
static void tbv_qp_ula_fallback_ambiguous_prefix(struct kunit *test)
{
	const u64 H = 0x0200544257524253ULL;
	const u64 D = 0x02005442aa55aa55ULL;
	const u64 LINK = 0xfd727599a7b95cb3ULL;
	struct dr_peer peers[2] = { { H, true }, { D, true } };
	struct dr_rail rails[2];
	u8 dgid[16];

	rails[0].peer_eui64 = H;
	dr_gid_rail_ula(rails[0].local_ula, LINK, 2);
	rails[1].peer_eui64 = D;
	dr_gid_rail_ula(rails[1].local_ula, LINK, 3);

	dr_gid_rail_ula(dgid, LINK, 1);
	KUNIT_EXPECT_EQ(test,
			dr_send_peer_after_rtr_v2(H, peers, 2, rails, 2, dgid),
			H);
}

static struct kunit_case tbv_qp_dgid_rebind_cases[] = {
	KUNIT_CASE(tbv_qp_send_follows_dgid_on_multipeer),
	KUNIT_CASE(tbv_qp_send_stable_when_home_is_dgid),
	KUNIT_CASE(tbv_qp_send_single_peer),
	KUNIT_CASE(tbv_qp_send_keeps_binding_on_unresolved_dgid),
	KUNIT_CASE(tbv_qp_send_follows_rail_ula_dgid_on_multipeer),
	KUNIT_CASE(tbv_qp_send_follows_rail_ula_when_identity_invalid),
	KUNIT_CASE(tbv_qp_ula_fallback_ignores_linklocal),
	KUNIT_CASE(tbv_qp_ula_fallback_ambiguous_prefix),
	{}
};

static struct kunit_suite tbv_qp_dgid_rebind_suite = {
	.name = "thunderbolt_ibverbs_qp_dgid_rebind",
	.test_cases = tbv_qp_dgid_rebind_cases,
};
kunit_test_suite(tbv_qp_dgid_rebind_suite);

MODULE_LICENSE("GPL");
