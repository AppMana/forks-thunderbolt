// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit regressions for live module replacement.
 *
 * These tests trace the ordered helpers used by the production rail-removal
 * and module-exit paths.  Keep lifecycle assertions here tied to those shared
 * sequencers: a separate teardown model previously stayed green while the
 * real module-exit order deadlocked before it reached the modeled fence.
 */
#include <kunit/test.h>
#include <linux/types.h>

#include "../tbv.h"

/*
 * Close admission and drain the current publisher before changing hardware
 * state.  The ICM tunnel must then be disconnected while its rings and hop IDs
 * still exist; only afterward may the rings be stopped and freed.  Stopping
 * rings first left a live fabric path pointing at dead DMA state, and the next
 * module load could publish descriptors that the NHI never consumed.
 */
static void tbv_hot_reload_disables_icm_before_stopping_rings(struct kunit *test)
{
	u8 steps[5] = {};
	u32 count = 0;
	int ret;

	ret = tbv_test_path_reload_order(steps, ARRAY_SIZE(steps), &count);

	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_ASSERT_EQ(test, count, (u32)ARRAY_SIZE(steps));
	KUNIT_EXPECT_EQ(test, steps[0],
			(u8)TBV_TEST_RELOAD_DISABLE_ADMISSION);
	KUNIT_EXPECT_EQ(test, steps[1],
			(u8)TBV_TEST_RELOAD_DRAIN_TX_SCHEDULER);
	KUNIT_EXPECT_EQ(test, steps[2],
			(u8)TBV_TEST_RELOAD_DISABLE_ICM_PATH);
	KUNIT_EXPECT_EQ(test, steps[3],
			(u8)TBV_TEST_RELOAD_STOP_RX_RING);
	KUNIT_EXPECT_EQ(test, steps[4],
			(u8)TBV_TEST_RELOAD_STOP_TX_RING);
}

/*
 * Module exit must close the registration gate, let service removal quiesce
 * and destroy every rail, and only then finalize the verbs subsystem.  The
 * previous exit called tbv_ibdev_stop() first; ib_unregister_device() could
 * wait for a QP whose completion needed the not-yet-fenced NHI ring, so exit
 * never reached tbv_services_stop().
 */
static void tbv_hot_reload_quiesces_services_before_ibdev_finalize(struct kunit *test)
{
	u8 steps[3] = {};
	u32 count = 0;
	int ret;

	ret = tbv_test_exit_reload_order(steps, ARRAY_SIZE(steps), &count);

	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_ASSERT_EQ(test, count, (u32)ARRAY_SIZE(steps));
	KUNIT_EXPECT_EQ(test, steps[0],
			(u8)TBV_TEST_RELOAD_IBDEV_QUIESCE);
	KUNIT_EXPECT_EQ(test, steps[1],
			(u8)TBV_TEST_RELOAD_SERVICES_STOP);
	KUNIT_EXPECT_EQ(test, steps[2],
			(u8)TBV_TEST_RELOAD_IBDEV_FINALIZE);
}

/* The unload waiter must not put a waitqueue lock in the live TX hot path. */
static void tbv_live_scheduler_only_wakes_during_quiesce(struct kunit *test)
{
	KUNIT_EXPECT_FALSE(test,
		tbv_test_path_scheduler_wake(TBV_PATH_TUNNEL_ENABLED));
	KUNIT_EXPECT_TRUE(test,
		tbv_test_path_scheduler_wake(TBV_PATH_RING_STARTED));
}

static struct kunit_case tbv_reload_quiesce_cases[] = {
	KUNIT_CASE(tbv_hot_reload_disables_icm_before_stopping_rings),
	KUNIT_CASE(tbv_hot_reload_quiesces_services_before_ibdev_finalize),
	KUNIT_CASE(tbv_live_scheduler_only_wakes_during_quiesce),
	{}
};

static struct kunit_suite tbv_reload_quiesce_suite = {
	.name = "thunderbolt_ibverbs_reload_quiesce",
	.test_cases = tbv_reload_quiesce_cases,
};
kunit_test_suite(tbv_reload_quiesce_suite);

MODULE_LICENSE("GPL");
