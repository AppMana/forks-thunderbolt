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

static struct kunit_case tbv_qp_dgid_rebind_cases[] = {
	KUNIT_CASE(tbv_qp_send_follows_dgid_on_multipeer),
	KUNIT_CASE(tbv_qp_send_stable_when_home_is_dgid),
	KUNIT_CASE(tbv_qp_send_single_peer),
	KUNIT_CASE(tbv_qp_send_keeps_binding_on_unresolved_dgid),
	{}
};

static struct kunit_suite tbv_qp_dgid_rebind_suite = {
	.name = "thunderbolt_ibverbs_qp_dgid_rebind",
	.test_cases = tbv_qp_dgid_rebind_cases,
};
kunit_test_suite(tbv_qp_dgid_rebind_suite);

MODULE_LICENSE("GPL");
