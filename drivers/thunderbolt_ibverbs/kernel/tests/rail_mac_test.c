// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for the rail netdev MAC / RAIL_EUI64 identity helpers
 * (kernel/native_control.c): tbv_host_identity_hash, tbv_rail_netdev_mac and
 * tbv_rail_identity_eui64.
 *
 * Properties: locally administered + unicast (mac[0] == 0x02), deterministic,
 * distinct per (host, peer, rail) -- including ACROSS hosts. The pre-0.2.35
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

static void tbv_test_rail_mac_laa_unicast(struct kunit *test)
{
	u8 m[6];

	tbv_rail_netdev_mac(tbv_host_identity_hash(host_a_uuid), 1, 0, m);
	KUNIT_EXPECT_EQ(test, 0x02, m[0]);     /* locally administered */
	KUNIT_EXPECT_EQ(test, 0, m[0] & 0x01); /* unicast, not multicast */
}

static void tbv_test_rail_mac_deterministic(struct kunit *test)
{
	u8 a[6], b[6];

	tbv_rail_netdev_mac(tbv_host_identity_hash(host_a_uuid), 2, 3, a);
	tbv_rail_netdev_mac(tbv_host_identity_hash(host_a_uuid), 2, 3, b);
	KUNIT_EXPECT_EQ(test, 0, memcmp(a, b, 6));
	/* the hash itself is stable and never zero */
	KUNIT_EXPECT_EQ(test, tbv_host_identity_hash(host_a_uuid),
			tbv_host_identity_hash(host_a_uuid));
	KUNIT_EXPECT_NE(test, 0u, tbv_host_identity_hash(host_a_uuid));
	KUNIT_EXPECT_NE(test, 0u, tbv_host_identity_hash(NULL));
}

static void tbv_test_rail_mac_distinct_within_host(struct kunit *test)
{
	u32 h = tbv_host_identity_hash(host_a_uuid);
	u8 p1r0[6], p2r0[6], p1r1[6];

	tbv_rail_netdev_mac(h, 1, 0, p1r0);
	tbv_rail_netdev_mac(h, 2, 0, p2r0);
	tbv_rail_netdev_mac(h, 1, 1, p1r1);
	KUNIT_EXPECT_NE(test, 0, memcmp(p1r0, p2r0, 6)); /* different peer */
	KUNIT_EXPECT_NE(test, 0, memcmp(p1r0, p1r1, 6)); /* different rail */
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

	tbv_rail_netdev_mac(tbv_host_identity_hash(host_a_uuid), 1, 0, a);
	tbv_rail_netdev_mac(tbv_host_identity_hash(host_b_uuid), 1, 0, b);
	KUNIT_EXPECT_NE(test, 0, memcmp(a, b, 6));
}

/*
 * The advertised RAIL_EUI64 identity is the modified-EUI-64 of the MAC: its
 * host part (>> 16) is shared by every rail of the host and encodes ff:fe in
 * the middle; the low 16 bits are exactly (peer_id, rail_id).
 */
static void tbv_test_rail_identity_eui64_shape(struct kunit *test)
{
	u32 h = tbv_host_identity_hash(host_a_uuid);
	u64 p1r0 = tbv_rail_identity_eui64(h, 1, 0);
	u64 p2r0 = tbv_rail_identity_eui64(h, 2, 0);
	u64 other = tbv_rail_identity_eui64(tbv_host_identity_hash(host_b_uuid),
					    1, 0);

	/* host part shared within a host, distinct across hosts */
	KUNIT_EXPECT_EQ(test, p1r0 >> 16, p2r0 >> 16);
	KUNIT_EXPECT_NE(test, p1r0 >> 16, other >> 16);
	/* low 16 bits are P:R */
	KUNIT_EXPECT_EQ(test, 0x0100ULL, p1r0 & 0xffff);
	KUNIT_EXPECT_EQ(test, 0x0200ULL, p2r0 & 0xffff);
	/* RFC 4291: U/L flipped 0x02 -> 0x00 top byte, ff:fe in bytes 3..4 */
	KUNIT_EXPECT_EQ(test, 0x00ULL, p1r0 >> 56);
	KUNIT_EXPECT_EQ(test, 0xfffeULL, (p1r0 >> 24) & 0xffff);
}

static struct kunit_case tbv_rail_mac_test_cases[] = {
	KUNIT_CASE(tbv_test_rail_mac_laa_unicast),
	KUNIT_CASE(tbv_test_rail_mac_deterministic),
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
