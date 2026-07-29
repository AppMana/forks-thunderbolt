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

static struct kunit_case tbv_max_message_capacity_cases[] = {
	KUNIT_CASE(tbv_max_message_fits_bounded_path_queue),
	{}
};

static struct kunit_suite tbv_max_message_capacity_suite = {
	.name = "thunderbolt_ibverbs_max_message_capacity",
	.test_cases = tbv_max_message_capacity_cases,
};
kunit_test_suite(tbv_max_message_capacity_suite);

MODULE_LICENSE("GPL");
