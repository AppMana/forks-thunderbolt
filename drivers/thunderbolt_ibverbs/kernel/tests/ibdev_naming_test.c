// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for thunderbolt_ibverbs per-rail ib_device naming
 * (tbv_ibdev_name_index in kernel/ibdev.c).
 *
 * Contract / the bug it pins: a host cabled to two neighbours reaches both
 * through the SAME tb domain (one controller -> one tb->index) on the SAME
 * native lane 0, but over DIFFERENT downstream adapters
 * (rail->key.local_adapter). The name index MUST differ between them, or the
 * second ib_register_device("usb4_rdmaN") returns -ENFILE (duplicate name) and
 * the second neighbour's rail never publishes an ib_device. Observed on
 * appmana-018 (cabled to 002 and 027): both rails -> "usb4_rdma0", second
 * registration failed with -23. Folding the local adapter into the index fixes
 * it while preserving lane disambiguation for a bonded link.
 *
 * Built into the module only on a CONFIG_KUNIT kernel (see kernel/Makefile).
 * The fleet's Ubuntu generic kernels lack CONFIG_KUNIT, so the same cases run
 * on-host via tests/ibdev_naming_userspace.c -- keep both in lockstep.
 */
#include <kunit/test.h>
#include "../tbv.h"

#define L TBV_NATIVE_MAX_LANES

static void tbv_naming_two_peers_one_domain_differ(struct kunit *test)
{
	/* rail facing a peer on adapter 1 vs a peer on adapter 3, both lane 0 */
	int a = tbv_ibdev_name_index(0, 1, 0, 0, L);
	int b = tbv_ibdev_name_index(0, 3, 0, 0, L);

	KUNIT_EXPECT_GE(test, a, 0);
	KUNIT_EXPECT_GE(test, b, 0);
	KUNIT_EXPECT_NE(test, a, b);
}

static void tbv_naming_deterministic(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, tbv_ibdev_name_index(0, 1, 0, 0, L),
			tbv_ibdev_name_index(0, 1, 0, 0, L));
}

static void tbv_naming_lanes_disambiguate(struct kunit *test)
{
	/* a bonded link's two lanes to one peer (same adapter) stay distinct */
	KUNIT_EXPECT_NE(test, tbv_ibdev_name_index(0, 1, 0, 0, L),
			tbv_ibdev_name_index(0, 1, 1, 0, L));
}

static void tbv_naming_apple_slot(struct kunit *test)
{
	KUNIT_EXPECT_NE(test, tbv_ibdev_name_index(0, 1, 0, 1, L),
			tbv_ibdev_name_index(0, 1, 0, 0, L));
	KUNIT_EXPECT_NE(test, tbv_ibdev_name_index(0, 2, 0, 1, L),
			tbv_ibdev_name_index(0, 1, 0, 1, L));
}

static void tbv_naming_domains_differ(struct kunit *test)
{
	KUNIT_EXPECT_NE(test, tbv_ibdev_name_index(1, 1, 0, 0, L),
			tbv_ibdev_name_index(0, 1, 0, 0, L));
}

static void tbv_naming_errors(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, tbv_ibdev_name_index(-1, 0, 0, 0, L), -ENODEV);
	KUNIT_EXPECT_EQ(test, tbv_ibdev_name_index(0, 0, L, 0, L), -ERANGE);
	KUNIT_EXPECT_EQ(test, tbv_ibdev_name_index(0, TBV_NAME_MAX_ADAPTERS, 0, 0, L),
			-ERANGE);
}

static struct kunit_case tbv_ibdev_naming_test_cases[] = {
	KUNIT_CASE(tbv_naming_two_peers_one_domain_differ),
	KUNIT_CASE(tbv_naming_deterministic),
	KUNIT_CASE(tbv_naming_lanes_disambiguate),
	KUNIT_CASE(tbv_naming_apple_slot),
	KUNIT_CASE(tbv_naming_domains_differ),
	KUNIT_CASE(tbv_naming_errors),
	{}
};

static struct kunit_suite tbv_ibdev_naming_test_suite = {
	.name = "thunderbolt_ibverbs_ibdev_naming",
	.test_cases = tbv_ibdev_naming_test_cases,
};
kunit_test_suite(tbv_ibdev_naming_test_suite);
