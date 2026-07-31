// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit: the path queue must be able to stage one message of the maximum size
 * query_port advertises. A smaller internal frame ceiling otherwise turns a
 * legal WR into permanent -ENOMEM retries followed by asynchronous timeout.
 */
#include <kunit/test.h>
#include "../tbv.h"

static void tbv_max_message_fits_bounded_path_queue(struct kunit *test)
{
	u32 required = 0;
	u32 limit = 0;

	KUNIT_ASSERT_EQ(test,
			tbv_test_max_message_frame_capacity(&required, &limit),
			0);
	KUNIT_EXPECT_GE_MSG(test, limit, required,
			    "advertised max message needs %u frames but path can reserve only %u",
			    required, limit);
}

/*
 * The production NCCL/perftest shape keeps 32 QPs at depth 128 with 256 KiB
 * WRs.  If the bounded path queue cannot hold that legal verbs working set,
 * every QP falls into the 10 ms timeout-worker capacity poll and the live
 * link drops from ~30 Gb/s to ~3.5 Gb/s despite having credits available.
 */
static void tbv_qps32_write_working_set_fits_path_queue(struct kunit *test)
{
	u32 required = 0;
	u32 limit = 0;

	KUNIT_ASSERT_EQ(test,
			tbv_test_qps32_write_queue_capacity(&required, &limit),
			0);
	KUNIT_EXPECT_GE_MSG(test, limit, required,
			    "qps32/depth128/256KiB needs %u frame slots but the path has %u; deferred QPs will poll capacity on their retransmit timers",
			    required, limit);
}

static struct kunit_case tbv_max_message_capacity_cases[] = {
	KUNIT_CASE(tbv_max_message_fits_bounded_path_queue),
	KUNIT_CASE(tbv_qps32_write_working_set_fits_path_queue),
	{}
};

static struct kunit_suite tbv_max_message_capacity_suite = {
	.name = "thunderbolt_ibverbs_max_message_capacity",
	.test_cases = tbv_max_message_capacity_cases,
};
kunit_test_suite(tbv_max_message_capacity_suite);

MODULE_LICENSE("GPL");
