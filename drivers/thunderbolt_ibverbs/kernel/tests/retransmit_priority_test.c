// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit: a peer-requested selective retransmit must not wait behind the entire
 * unsent initial-data backlog on the shared path.
 */
#include <kunit/test.h>
#include "../tbv.h"

static void tbv_retransmit_preempts_unsent_initial_data(struct kunit *test)
{
	u32 first = 0;
	u32 second = 0;
	u32 third = 0;

	KUNIT_ASSERT_EQ(test,
			tbv_test_path_retransmit_priority(&first, &second,
							  &third),
			0);
	KUNIT_EXPECT_EQ_MSG(test, first, 3U,
			    "selective retransmit remained behind initial data");
	KUNIT_EXPECT_EQ(test, second, 1U);
	KUNIT_EXPECT_EQ(test, third, 2U);
}

static struct kunit_case tbv_retransmit_priority_cases[] = {
	KUNIT_CASE(tbv_retransmit_preempts_unsent_initial_data),
	{}
};

static struct kunit_suite tbv_retransmit_priority_suite = {
	.name = "thunderbolt_ibverbs_retransmit_priority",
	.test_cases = tbv_retransmit_priority_cases,
};

kunit_test_suite(tbv_retransmit_priority_suite);
