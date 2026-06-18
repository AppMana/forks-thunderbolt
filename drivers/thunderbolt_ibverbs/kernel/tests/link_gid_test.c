// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for the honest-HCA per-link GID /64 derivation
 * (tbv_link_gid_prefix in kernel/native_control.c).
 *
 * Contract: given the two hosts' Thunderbolt unique_id UUIDs on a link, derive
 * the RoCE GID /64 subnet prefix so that it is (a) symmetric -- both ends derive
 * the same /64, so they share a subnet and a dest GID matches by subnet; (b)
 * distinct per link -- a node's two cabled links get different /64s; (c) ULA.
 *
 * Mirrored verbatim in kernel/tests/link_gid_userspace.c for on-host runs
 * (fleet kernels lack CONFIG_KUNIT). Change all three together.
 *
 * Build: included only when CONFIG_KUNIT is set (see kernel/Makefile).
 */
#include <kunit/test.h>
#include <linux/string.h>
#include "../tbv.h"

static const u8 UA[16] = { 0xc1,0x01,0,0,0,0x80,0x84,0x1e,
			   0x03,0xb7,0xb9,0x1c,0xd8,0x23,0x20,0x23 };
static const u8 UB[16] = { 0x5a,0x11,0x22,0xb7,0x77,0x15,0,0,0,0,0,0,0,0,0,0x01 };
static const u8 UC[16] = { 0x5a,0x11,0x22,0xb7,0x75,0xb1,0,0,0,0,0,0,0,0,0,0x02 };

static void tbv_test_link_gid_symmetric(struct kunit *test)
{
	u8 ab[8], ba[8];

	tbv_link_gid_prefix(UA, UB, ab);
	tbv_link_gid_prefix(UB, UA, ba);
	KUNIT_EXPECT_EQ(test, 0, memcmp(ab, ba, 8));
}

static void tbv_test_link_gid_is_ula(struct kunit *test)
{
	u8 ab[8], ac[8];

	tbv_link_gid_prefix(UA, UB, ab);
	tbv_link_gid_prefix(UA, UC, ac);
	KUNIT_EXPECT_EQ(test, 0xfd, ab[0]);
	KUNIT_EXPECT_EQ(test, 0xfd, ac[0]);
}

static void tbv_test_link_gid_distinct_per_link(struct kunit *test)
{
	u8 ab[8], ac[8], bc[8];

	tbv_link_gid_prefix(UA, UB, ab);
	tbv_link_gid_prefix(UA, UC, ac);
	tbv_link_gid_prefix(UB, UC, bc);
	/* node A's two links differ; all three links mutually differ */
	KUNIT_EXPECT_NE(test, 0, memcmp(ab, ac, 8));
	KUNIT_EXPECT_NE(test, 0, memcmp(ab, bc, 8));
	KUNIT_EXPECT_NE(test, 0, memcmp(ac, bc, 8));
}

static struct kunit_case tbv_link_gid_test_cases[] = {
	KUNIT_CASE(tbv_test_link_gid_symmetric),
	KUNIT_CASE(tbv_test_link_gid_is_ula),
	KUNIT_CASE(tbv_test_link_gid_distinct_per_link),
	{}
};

static struct kunit_suite tbv_link_gid_test_suite = {
	.name = "tbv_link_gid",
	.test_cases = tbv_link_gid_test_cases,
};
kunit_test_suite(tbv_link_gid_test_suite);

MODULE_LICENSE("GPL");
