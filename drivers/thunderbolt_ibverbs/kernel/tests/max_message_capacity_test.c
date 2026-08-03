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
 * The production NCCL/perftest shape keeps 32 QPs at depth 128.  Those WQEs
 * belong on the verbs SQ as lightweight send contexts; they must not all be
 * expanded into hundreds of path packets before the hardware can consume
 * them.  Besides making queue memory scale with a userspace working set, that
 * design exhausted the path queue at QP 9--10 when the payload grew from the
 * old 256 KiB test shape to 1 MiB.  The local -ENOMEM then escaped as a fatal
 * post_send error.
 *
 * A bounded path queue must fit one advertised maximum-size message (covered
 * above), while the full qps32/depth128 working set must remain larger and be
 * handled by deferred packetization/backpressure.
 */
static void tbv_qps32_write_working_set_is_deferred(struct kunit *test)
{
	u32 required = 0;
	u32 limit = 0;

	KUNIT_ASSERT_EQ(test,
			tbv_test_qps32_write_queue_capacity(&required, &limit),
			0);
	KUNIT_EXPECT_GT_MSG(test, required, limit,
			    "path queue pre-packetizes the entire qps32/depth128 working set (%u frames in %u slots) instead of applying bounded backpressure",
			    required, limit);
}

static struct kunit_case tbv_max_message_capacity_cases[] = {
	KUNIT_CASE(tbv_max_message_fits_bounded_path_queue),
	KUNIT_CASE(tbv_qps32_write_working_set_is_deferred),
	{}
};

static struct kunit_suite tbv_max_message_capacity_suite = {
	.name = "thunderbolt_ibverbs_max_message_capacity",
	.test_cases = tbv_max_message_capacity_cases,
};
kunit_test_suite(tbv_max_message_capacity_suite);

MODULE_LICENSE("GPL");
