// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for tbv_rail_netdev_mac (kernel/native_control.c): the MAC of a
 * rail's private GID-only netdev. Properties: locally administered + unicast
 * (mac[0] == 0x02), deterministic, and distinct per rail (distinct node_guid
 * low bits -> distinct MAC -> distinct per-rail RoCE GID).
 *
 * Mirrored in kernel/tests/rail_mac_userspace.c for on-host runs (fleet kernels
 * lack CONFIG_KUNIT). Compiled only when CONFIG_KUNIT is set (see kernel/Makefile).
 */
#include <kunit/test.h>
#include <linux/string.h>
#include "../tbv.h"

/* node_guid = 0x0200544256524253 + (peer_id<<24) + rail_id (tbv_ibdev_register) */
#define GUID(peer, rail) (0x0200544256524253ULL + ((u64)(peer) << 24) + (rail))

static void tbv_test_rail_mac_laa_unicast(struct kunit *test)
{
	u8 m[6];

	tbv_rail_netdev_mac(GUID(1, 0), m);
	KUNIT_EXPECT_EQ(test, 0x02, m[0]);     /* locally administered */
	KUNIT_EXPECT_EQ(test, 0, m[0] & 0x01); /* unicast, not multicast */
}

static void tbv_test_rail_mac_deterministic(struct kunit *test)
{
	u8 a[6], b[6];

	tbv_rail_netdev_mac(GUID(2, 3), a);
	tbv_rail_netdev_mac(GUID(2, 3), b);
	KUNIT_EXPECT_EQ(test, 0, memcmp(a, b, 6));
}

static void tbv_test_rail_mac_distinct(struct kunit *test)
{
	u8 p1r0[6], p2r0[6], p1r1[6];

	tbv_rail_netdev_mac(GUID(1, 0), p1r0);
	tbv_rail_netdev_mac(GUID(2, 0), p2r0);
	tbv_rail_netdev_mac(GUID(1, 1), p1r1);
	KUNIT_EXPECT_NE(test, 0, memcmp(p1r0, p2r0, 6)); /* different peer */
	KUNIT_EXPECT_NE(test, 0, memcmp(p1r0, p1r1, 6)); /* different rail */
}

static struct kunit_case tbv_rail_mac_test_cases[] = {
	KUNIT_CASE(tbv_test_rail_mac_laa_unicast),
	KUNIT_CASE(tbv_test_rail_mac_deterministic),
	KUNIT_CASE(tbv_test_rail_mac_distinct),
	{}
};

static struct kunit_suite tbv_rail_mac_test_suite = {
	.name = "tbv_rail_mac",
	.test_cases = tbv_rail_mac_test_cases,
};
kunit_test_suite(tbv_rail_mac_test_suite);

MODULE_LICENSE("GPL");
