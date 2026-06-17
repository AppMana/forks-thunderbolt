// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for thunderbolt_ibverbs per-rail ib_device naming
 * (tbv_ibdev_name_index in kernel/ibdev.c).
 *
 * Models the REAL inputs seen on a mid-chain node, captured live on
 * appmana-022 (cabled to 021 and 023). Both native rails report the SAME
 * (domain 0, native_lane 0, local_adapter 0); only the XDomain ROUTE differs
 * (downstream port 0x3 vs 0x1). The current index keys on
 * (domain, local_adapter, lane) and never looks at the route, so both rails
 * map to usb4_rdma0 and the second ib_register_device() returns -ENFILE (-23):
 *   registered native ib_device usb4_rdma0 ... route=0x3
 *   failed to register per-rail ib_device usb4_rdma0: -23 ... route=0x1
 *
 * This suite FAILS against the current logic (the bug is real, on hardware) and
 * passes once the index keys on the route. Keep in lockstep with the userspace
 * mirror tests/ibdev_naming_userspace.c (the fleet kernels lack CONFIG_KUNIT).
 */
#include <kunit/test.h>
#include "../tbv.h"

#define L TBV_NATIVE_MAX_LANES

/*
 * appmana-022's two native rails as tbv_ibdev_rail_name_index() reads them off
 * each struct tbv_rail: identical domain/local_adapter/native_lane, differing
 * only in rail->key.route, whose low byte is the downstream port (0x3 vs 0x1).
 * Keying the index on that port gives them distinct names.
 */
static void tbv_naming_two_neighbours_must_differ(struct kunit *test)
{
	/* rail to 021, route 0x3 -> port 3 */
	int a = tbv_ibdev_name_index(0, /*route_port=*/0x3, /*lane=*/0, /*apple=*/0, L);
	/* rail to 023, route 0x1 -> port 1 */
	int b = tbv_ibdev_name_index(0, /*route_port=*/0x1, /*lane=*/0, /*apple=*/0, L);

	KUNIT_EXPECT_GE(test, a, 0);
	KUNIT_EXPECT_GE(test, b, 0);
	/* MUST differ or the 2nd ib_register_device() is -ENFILE */
	KUNIT_EXPECT_NE(test, a, b);
}

static void tbv_naming_lanes_disambiguate(struct kunit *test)
{
	/* a bonded link's two lanes to one peer must still get distinct names */
	KUNIT_EXPECT_NE(test, tbv_ibdev_name_index(0, 0, 0, 0, L),
			tbv_ibdev_name_index(0, 0, 1, 0, L));
}

static void tbv_naming_errors(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, tbv_ibdev_name_index(-1, 0, 0, 0, L), -ENODEV);
	KUNIT_EXPECT_EQ(test, tbv_ibdev_name_index(0, 0, L, 0, L), -ERANGE);
}

static struct kunit_case tbv_ibdev_naming_test_cases[] = {
	KUNIT_CASE(tbv_naming_two_neighbours_must_differ),
	KUNIT_CASE(tbv_naming_lanes_disambiguate),
	KUNIT_CASE(tbv_naming_errors),
	{}
};

static struct kunit_suite tbv_ibdev_naming_test_suite = {
	.name = "thunderbolt_ibverbs_ibdev_naming",
	.test_cases = tbv_ibdev_naming_test_cases,
};
kunit_test_suite(tbv_ibdev_naming_test_suite);
