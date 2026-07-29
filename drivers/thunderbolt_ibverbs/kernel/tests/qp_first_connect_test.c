// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit: the FIRST data-path exchange of a freshly connected native QP must
 * complete on a directly-cabled adjacent pair -- the 2026-07-11 silent NCCL
 * first-connect hang (appmana-002 endpoint <-> appmana-018 interior).
 *
 * Hardware signature: NCCL init completes, 6 channels build, the proxy hangs
 * on the first exchange with NO WC error and (apparently) nothing in dmesg.
 * Live counters told the real story: 018 accepted 40 WRs and enqueued 578
 * data frames on PEER 2's rail (route 0x3, its OTHER neighbour) while peer
 * 1's rail (route 0x1, appmana-002) carried ZERO data frames; 002 received
 * nothing (data_rx_send=0), so its proxy polled the shared-FIFO memory
 * forever. 018's "native retransmit" and "tx stall" warns WERE in dmesg --
 * ibdev.c/path.c have no pr_fmt prefix, so `dmesg | grep thunderbolt_ibverbs`
 * missed them.
 *
 * Root cause chain:
 *   1. NCCL created rank1's QP on the ib_device homed on the WRONG neighbour
 *      (a mid-chain node has one ib_device per rail; the QP's create-time
 *      rail is that device's home peer -- 0.2.33 semantics).
 *   2. The 0.2.34 fix for exactly this -- tbv_qp_rebind_to_dgid() at
 *      modify_qp(RTR) -- NEVER ENGAGED: with no pinned roce_netdev (the
 *      fleet default; the param is an rxe_lan-era knob) every HELLO
 *      advertised identity (0, 0), so no peer ever had
 *      remote_identity_valid and tbv_peer_for_dgid_locked() could not match
 *      ANY dgid. Fleet-wide no-op.
 *   3. Even had a per-rail netdev identity been advertised, the pre-0.2.35
 *      MAC derivation (node_guid low bits = LOCAL peer_id/rail_id only) gave
 *      every node's "peer 1" rail the SAME MAC -- 002's GID and 018's
 *      usb4_rdma5 GID were literally identical -- so identities could
 *      collide across neighbours and dgids were not attributable.
 *
 * The fix: (a) rail netdev MACs get a host-unique 24-bit hash
 * (tbv_rail_netdev_mac / tbv_host_identity_hash), (b) when no roce_netdev is
 * pinned, each HELLO advertises the rail's synthetic modified-EUI-64 flagged
 * TBV_NATIVE_WIRE_CAP_RAIL_EUI64, and (c) the resolver host-part-matches
 * such identities (tbv_gid_rail_identity_verdict, eui64 >> 16) so EVERY GID
 * of the peer's node resolves, whichever of its ib_devices NCCL took it from.
 *
 * These tests model create_qp (home-rail bind) -> modify_qp(RTR) (dgid
 * rebind) -> first send using the REAL identity helpers and verdicts;
 * "the first exchange completes" == the send egresses the rail of the peer
 * that owns the dgid (the responder then receives, ACKs return on the
 * arrival rail, and the WC is delivered -- route consistency, as pinned by
 * qp_dgid_rebind_test.c). TBV_HELLO_RAIL_IDENTITY is the lockstep lever:
 * built =0 it reproduces the zero-identity HELLOs and the misrouted first
 * send (RED). Run via tools/run-kunit.sh.
 */
#include <kunit/test.h>
#include <linux/string.h>
#include <linux/types.h>
#include "../../proto/native_wire.h"
#include "../tbv.h"

/* Lockstep with tbv_native_control_fill_hello(): 1 = a node with no pinned
 * roce_netdev advertises its rail's synthetic RAIL_EUI64 identity; 0 =
 * pre-fix (HELLO carries identity (0, 0) -> peers never resolve any dgid). */
#ifndef TBV_HELLO_RAIL_IDENTITY
#define TBV_HELLO_RAIL_IDENTITY 1
#endif

/* Host router UUIDs: the endpoint (002), the interior node under test (018),
 * and 018's other neighbour (X). */
static const u8 fc_uuid_002[16] = {
	0x3c, 0xa1, 0x55, 0x0e, 0x81, 0x29, 0x4b, 0x07,
	0xb4, 0x1f, 0x6d, 0x92, 0xe8, 0x03, 0x5a, 0xc6,
};
static const u8 fc_uuid_018[16] = {
	0x7e, 0x12, 0xc9, 0x64, 0x0b, 0xd5, 0x48, 0x93,
	0xa0, 0x8e, 0x27, 0x4c, 0xf1, 0x66, 0x39, 0x0d,
};
static const u8 fc_uuid_x[16] = {
	0x51, 0xf7, 0x02, 0xbd, 0x6e, 0x38, 0x4a, 0xe5,
	0x9c, 0x40, 0xd3, 0x1b, 0x87, 0x2f, 0xa4, 0x72,
};

/* One neighbour as the local node stores it after the HELLO exchange:
 * mirror of tbv_native_control_apply_remote()'s identity block. */
struct fc_peer {
	u64 hello_eui64;	/* what the neighbour's HELLO advertised */
	u32 hello_ipv4;
	bool rail_scoped;	/* HELLO carried TBV_NATIVE_WIRE_CAP_RAIL_EUI64 */
	bool valid;		/* remote_identity_valid */
};

/*
 * Mirror of tbv_native_control_fill_hello()'s identity block for a node with
 * NO pinned roce_netdev (the fleet default): under the lever it advertises
 * the rail's synthetic identity via the REAL helpers; pre-fix it advertises
 * (0, 0) and the receiving peer never validates the identity.
 */
static void fc_hello(struct fc_peer *stored, const u8 sender_uuid[16],
		     u32 sender_peer_id, u32 sender_rail_id)
{
	memset(stored, 0, sizeof(*stored));
	if (TBV_HELLO_RAIL_IDENTITY) {
		stored->hello_eui64 = tbv_rail_identity_eui64(
			tbv_host_identity_hash(sender_uuid),
			tbv_rail_link_identity_hash(sender_uuid,
						   sender_peer_id,
						   sender_rail_id));
		stored->rail_scoped = true;
	}
	/* apply_remote: identity valid iff anything was advertised */
	stored->valid = stored->hello_eui64 || stored->hello_ipv4;
}

/* A link-local GID of @host's (peer_id, rail_id) rail netdev, exactly the
 * shape ib_core builds from the MAC the fix derives. */
static void fc_gid(u8 gid[16], const u8 host_uuid[16], u32 peer_id,
		   u32 rail_id)
{
	u64 eui64 = tbv_rail_identity_eui64(
		tbv_host_identity_hash(host_uuid),
		tbv_rail_link_identity_hash(host_uuid, peer_id, rail_id));
	int i;

	memset(gid, 0, 16);
	gid[0] = 0xfe;
	gid[1] = 0x80;
	for (i = 0; i < 8; i++)
		gid[8 + i] = (u8)(eui64 >> (56 - 8 * i));
}

/* Mirror of tbv_peer_for_dgid_locked() using the REAL verdicts: the index of
 * the single conclusively-matching peer, or -1 (unknown / ambiguous). */
static int fc_resolve(const struct fc_peer *peers, int n, const u8 dgid[16])
{
	int match = -1;
	int i;

	for (i = 0; i < n; i++) {
		enum tbv_identity_verdict verdict;

		if (!peers[i].valid)
			continue;
		if (peers[i].rail_scoped)
			verdict = tbv_gid_rail_identity_verdict(
				dgid, peers[i].hello_eui64);
		else
			verdict = tbv_gid_identity_verdict(
				dgid, true, peers[i].hello_eui64,
				peers[i].hello_ipv4);
		if (verdict != TBV_IDENTITY_MATCH)
			continue;
		if (match >= 0 && match != i)
			return -1;	/* ambiguous */
		match = i;
	}
	return match;
}

/*
 * The QP lifecycle under test: create_qp binds the chosen ib_device's home
 * peer (@home); modify_qp(RTR) rebinds to the dgid's peer when it resolves
 * (tbv_qp_rebind_to_dgid), else keeps the create-time binding. Returns the
 * peer index the first post-RTS send egresses on.
 */
static int fc_send_peer_after_connect(int home, const struct fc_peer *peers,
				      int n, const u8 dgid[16])
{
	int dest = fc_resolve(peers, n, dgid);

	return dest >= 0 ? dest : home;
}

/*
 * THE HANG. On 018: peers { 002, X }; NCCL created the QP on the X-homed
 * ib_device; the RTR dgid is a GID of 002 (which numbers 018 as ITS peer 1,
 * rail 0). The first send must egress 002's rail -- then 002 receives the
 * shared-FIFO write, ACKs return on the arrival rail, and the WC is
 * delivered. Pre-fix (lever=0) both HELLOs advertised (0, 0): the dgid
 * resolves nowhere, the QP keeps the X binding, and every frame egresses the
 * wrong neighbour -- 578 frames on route 0x3, zero on route 0x1, no WC,
 * NCCL hangs. RED with TBV_HELLO_RAIL_IDENTITY=0.
 */
static void tbv_first_connect_send_reaches_dgid_peer(struct kunit *test)
{
	struct fc_peer peers[2];
	u8 dgid[16];
	int send_peer;

	/* 002 numbers 018 "peer 1"; X numbers 018 "peer 2" */
	fc_hello(&peers[0], fc_uuid_002, 1, 0);	/* peer 0 = 002 */
	fc_hello(&peers[1], fc_uuid_x, 2, 0);	/* peer 1 = X */
	fc_gid(dgid, fc_uuid_002, 1, 0);	/* NCCL connects to 002 */

	send_peer = fc_send_peer_after_connect(1 /* created X-homed */,
					       peers, 2, dgid);
	/* first exchange completes iff the send egresses the dgid's peer */
	KUNIT_EXPECT_EQ(test, 0, send_peer);
	KUNIT_EXPECT_EQ(test, 0, fc_resolve(peers, 2, dgid));
}

/*
 * NCCL may hand out a GID from ANY of the destination node's ib_devices, not
 * just the one on the shared link: the dgid's low 16 identity bits then carry
 * a physical-link hash we never saw. Host-part matching must still resolve it.
 * (On 002 this is literally the repro's other direction: rank1's GID came
 * from 018's usb4_rdma15, the X-facing device.)
 */
static void tbv_first_connect_resolves_cross_device_gid(struct kunit *test)
{
	struct fc_peer peers[1];
	u8 dgid[16];

	/* 018's HELLO to 002 rode its 002-facing link... */
	fc_hello(&peers[0], fc_uuid_018, 1, 0);
	/* ...but the dgid names 018's X-facing link. */
	fc_gid(dgid, fc_uuid_018, 2, 0);

	KUNIT_EXPECT_EQ(test, 0,
			fc_send_peer_after_connect(0, peers, 1, dgid));
	KUNIT_EXPECT_EQ(test, 0, fc_resolve(peers, 1, dgid));
}

/*
 * Both neighbours number the local node "peer 1, rail 0" -- under the old
 * node_guid MAC scheme their identities (and GIDs!) were byte-identical and
 * any resolver must return "ambiguous". The host hash makes them distinct, so
 * the dgid resolves uniquely.
 */
static void tbv_first_connect_same_numbering_not_ambiguous(struct kunit *test)
{
	struct fc_peer peers[2];
	u8 dgid[16];

	fc_hello(&peers[0], fc_uuid_002, 1, 0);
	fc_hello(&peers[1], fc_uuid_x, 1, 0);	/* same link tag as 002's */
	fc_gid(dgid, fc_uuid_x, 1, 0);

	KUNIT_EXPECT_EQ(test, 1, fc_resolve(peers, 2, dgid));
	KUNIT_EXPECT_EQ(test, 1,
			fc_send_peer_after_connect(0, peers, 2, dgid));
}

/*
 * A dgid of an unknown host (not a Thunderbolt neighbour: the node's own
 * loopback/flush QP, or a chain wrap destination) must keep the create-time
 * binding and never fail RTR. A v4-mapped dgid abstains rather than
 * mismatching (rail identities carry no IPv4).
 */
static void tbv_first_connect_unresolved_keeps_home(struct kunit *test)
{
	static const u8 unknown_uuid[16] = {
		0xde, 0xad, 0xbe, 0xef, 0x00, 0x11, 0x22, 0x33,
		0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb,
	};
	struct fc_peer peers[2];
	u8 dgid[16];
	u8 v4gid[16] = { [10] = 0xff, [11] = 0xff,
			 [12] = 10, [13] = 2, [14] = 0, [15] = 7 };

	fc_hello(&peers[0], fc_uuid_002, 1, 0);
	fc_hello(&peers[1], fc_uuid_x, 2, 0);

	fc_gid(dgid, unknown_uuid, 1, 0);
	KUNIT_EXPECT_EQ(test, -1, fc_resolve(peers, 2, dgid));
	KUNIT_EXPECT_EQ(test, 1,
			fc_send_peer_after_connect(1, peers, 2, dgid));

	if (TBV_HELLO_RAIL_IDENTITY) {
		KUNIT_EXPECT_EQ(test, TBV_IDENTITY_INCONCLUSIVE,
				tbv_gid_rail_identity_verdict(
					v4gid, peers[0].hello_eui64));
	}
}

static struct kunit_case tbv_qp_first_connect_cases[] = {
	KUNIT_CASE(tbv_first_connect_send_reaches_dgid_peer),
	KUNIT_CASE(tbv_first_connect_resolves_cross_device_gid),
	KUNIT_CASE(tbv_first_connect_same_numbering_not_ambiguous),
	KUNIT_CASE(tbv_first_connect_unresolved_keeps_home),
	{}
};

static struct kunit_suite tbv_qp_first_connect_suite = {
	.name = "thunderbolt_ibverbs_qp_first_connect",
	.test_cases = tbv_qp_first_connect_cases,
};
kunit_test_suite(tbv_qp_first_connect_suite);

MODULE_LICENSE("GPL");
