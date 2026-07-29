// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for the rail netdev MAC / RAIL_EUI64 identity helpers
 * (kernel/native_control.c): tbv_host_identity_hash, tbv_rail_netdev_mac and
 * tbv_rail_identity_eui64.
 *
 * Properties: locally administered + unicast (mac[0] == 0x02), deterministic,
 * stable across transient peer re-registration, and distinct per
 * (local host, remote host/route, rail) -- including ACROSS hosts. The pre-0.2.35
 * derivation took the ib node_guid's low 40 bits, whose only variables were
 * the LOCAL peer_id/rail_id: every node's "peer 1" rail carried the SAME MAC
 * fleet-wide (02:42:57:52:42:53 on both appmana-002 and appmana-018), so the
 * GIDs never identified a node and no HELLO identity could resolve them --
 * root soil of the 2026-07-11 silent first-connect NCCL hang. The host hash
 * (folded from the stable host router UUID) is what makes the identity
 * node-unique; the eui64's host part (>> 16) is shared by every rail of one
 * host and by no rail of another.
 *
 * Run via drivers/thunderbolt_ibverbs/tools/run-kunit.sh (kunit.py against an
 * overlaid v6.17 tree); built into the module on a CONFIG_KUNIT kernel.
 */
#include <kunit/test.h>
#include <linux/string.h>
#include "../tbv.h"

/* Two distinct host router UUIDs (the tb_xdomain local_uuid bytes). */
static const u8 host_a_uuid[16] = {
	0x9f, 0x02, 0x11, 0x5c, 0x3a, 0x77, 0x41, 0xd2,
	0x88, 0x04, 0xe6, 0x2b, 0x19, 0xc5, 0x70, 0x3e,
};
static const u8 host_b_uuid[16] = {
	0x1b, 0xe4, 0x9d, 0x08, 0x52, 0xa1, 0x4f, 0x66,
	0x93, 0x7c, 0x0d, 0xf8, 0x24, 0x6a, 0xb1, 0x57,
};
static const u8 host_c_uuid[16] = {
	0x61, 0xa3, 0x48, 0x7e, 0x14, 0x2b, 0x45, 0xcd,
	0xb9, 0x20, 0x34, 0x82, 0xfe, 0x79, 0x0a, 0xd1,
};

static void rail_mac(const u8 local_uuid[16], const u8 remote_uuid[16],
		     u64 route, u32 rail_id, u8 mac[6])
{
	tbv_rail_netdev_mac(tbv_host_identity_hash(local_uuid),
			    tbv_rail_link_identity_hash(remote_uuid, route,
							rail_id),
			    mac);
}

static void tbv_test_rail_mac_laa_unicast(struct kunit *test)
{
	u8 m[6];

	rail_mac(host_a_uuid, host_b_uuid, 1, 0, m);
	KUNIT_EXPECT_EQ(test, 0x02, m[0]);     /* locally administered */
	KUNIT_EXPECT_EQ(test, 0, m[0] & 0x01); /* unicast, not multicast */
}

static void tbv_test_rail_mac_deterministic(struct kunit *test)
{
	u8 a[6], b[6];

	rail_mac(host_a_uuid, host_b_uuid, 2, 3, a);
	rail_mac(host_a_uuid, host_b_uuid, 2, 3, b);
	KUNIT_EXPECT_EQ(test, 0, memcmp(a, b, 6));
	/* the hash itself is stable and never zero */
	KUNIT_EXPECT_EQ(test, tbv_host_identity_hash(host_a_uuid),
			tbv_host_identity_hash(host_a_uuid));
	KUNIT_EXPECT_NE(test, 0u, tbv_host_identity_hash(host_a_uuid));
	KUNIT_EXPECT_NE(test, 0u, tbv_host_identity_hash(NULL));
}

/*
 * Re-registering the same live XDomain allocates a fresh transient peer_id.
 * That implementation detail must not change the netdev MAC, because ib_core
 * derives the rail's link-local GID from it.
 */
static void tbv_test_rail_mac_stable_across_peer_reregister(struct kunit *test)
{
	u32 peer_id_before = 2;
	u32 peer_id_after = 5;
	u8 before[6], after[6];

	/*
	 * The registration records differ only in their transient peer_id.
	 * rail_mac() deliberately has no peer_id input, making that allocator
	 * state unable to perturb the identity.
	 */
	KUNIT_ASSERT_NE(test, peer_id_before, peer_id_after);
	rail_mac(host_a_uuid, host_b_uuid, 1, 0, before);
	rail_mac(host_a_uuid, host_b_uuid, 1, 0, after);
	KUNIT_EXPECT_EQ(test, 0, memcmp(before, after, 6));
}

static void tbv_test_rail_mac_distinct_within_host(struct kunit *test)
{
	u8 peer_b[6], peer_c[6], other_route[6], other_rail[6];

	rail_mac(host_a_uuid, host_b_uuid, 1, 0, peer_b);
	rail_mac(host_a_uuid, host_c_uuid, 1, 0, peer_c);
	rail_mac(host_a_uuid, host_b_uuid, 3, 0, other_route);
	rail_mac(host_a_uuid, host_b_uuid, 1, 1, other_rail);
	KUNIT_EXPECT_NE(test, 0, memcmp(peer_b, peer_c, 6));
	KUNIT_EXPECT_NE(test, 0, memcmp(peer_b, other_route, 6));
	KUNIT_EXPECT_NE(test, 0, memcmp(peer_b, other_rail, 6));
}

/*
 * THE FLEET COLLISION: two hosts that both number their neighbour "peer 1,
 * rail 0" must NOT share a MAC/GID. The node_guid-based scheme did exactly
 * that (identical low-40 bits), which is why appmana-002 and appmana-018
 * carried the same link-local GID and no dgid was ever attributable.
 */
static void tbv_test_rail_mac_distinct_across_hosts(struct kunit *test)
{
	u8 a[6], b[6];

	rail_mac(host_a_uuid, host_c_uuid, 1, 0, a);
	rail_mac(host_b_uuid, host_c_uuid, 1, 0, b);
	KUNIT_EXPECT_NE(test, 0, memcmp(a, b, 6));
}

/*
 * The advertised RAIL_EUI64 identity is the modified-EUI-64 of the MAC: its
 * host part (>> 16) is shared by every rail of the host and encodes ff:fe in
 * the middle; the low 16 bits are the stable physical-link identity.
 */
static void tbv_test_rail_identity_eui64_shape(struct kunit *test)
{
	u32 h = tbv_host_identity_hash(host_a_uuid);
	u16 link_b = tbv_rail_link_identity_hash(host_b_uuid, 1, 0);
	u16 link_c = tbv_rail_link_identity_hash(host_c_uuid, 1, 0);
	u64 p1r0 = tbv_rail_identity_eui64(h, link_b);
	u64 p2r0 = tbv_rail_identity_eui64(h, link_c);
	u64 other = tbv_rail_identity_eui64(
		tbv_host_identity_hash(host_b_uuid), link_b);

	/* host part shared within a host, distinct across hosts */
	KUNIT_EXPECT_EQ(test, p1r0 >> 16, p2r0 >> 16);
	KUNIT_EXPECT_NE(test, p1r0 >> 16, other >> 16);
	/* low 16 bits carry the link hash and remain distinct */
	KUNIT_EXPECT_EQ(test, (u64)link_b, p1r0 & 0xffff);
	KUNIT_EXPECT_EQ(test, (u64)link_c, p2r0 & 0xffff);
	KUNIT_EXPECT_NE(test, p1r0 & 0xffff, p2r0 & 0xffff);
	/* RFC 4291: U/L flipped 0x02 -> 0x00 top byte, ff:fe in bytes 3..4 */
	KUNIT_EXPECT_EQ(test, 0x00ULL, p1r0 >> 56);
	KUNIT_EXPECT_EQ(test, 0xfffeULL, (p1r0 >> 24) & 0xffff);
}

static struct kunit_case tbv_rail_mac_test_cases[] = {
	KUNIT_CASE(tbv_test_rail_mac_laa_unicast),
	KUNIT_CASE(tbv_test_rail_mac_deterministic),
	KUNIT_CASE(tbv_test_rail_mac_stable_across_peer_reregister),
	KUNIT_CASE(tbv_test_rail_mac_distinct_within_host),
	KUNIT_CASE(tbv_test_rail_mac_distinct_across_hosts),
	KUNIT_CASE(tbv_test_rail_identity_eui64_shape),
	{}
};

static struct kunit_suite tbv_rail_mac_test_suite = {
	.name = "tbv_rail_mac",
	.test_cases = tbv_rail_mac_test_cases,
};
kunit_test_suite(tbv_rail_mac_test_suite);

MODULE_LICENSE("GPL");
