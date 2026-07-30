// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit: path packet capacity is not send-queue capacity.
 *
 * The hardware capture posted 1 MiB WRs to a QP created with 128 send WRs.
 * Four WRs occupied roughly 1,040 transport frames, exhausting the current
 * path budget; ibv_post_send then returned -ENOMEM synchronously although the
 * verbs SQ still had 124 legal WQEs. Consumers treat that as a fatal contract
 * violation. A legal WR must stay on the SQ and be packetized after earlier
 * frames drain.
 */
#include <kunit/test.h>
#include "../tbv.h"

static void tbv_initial_send_capacity_is_deferred(struct kunit *test)
{
	int post_ret = -1;
	bool pending = false;
	bool deferred = false;

	KUNIT_ASSERT_EQ(test,
			tbv_test_initial_send_capacity_deferred(&post_ret,
							       &pending,
							       &deferred),
			0);
	KUNIT_EXPECT_EQ_MSG(test, post_ret, 0,
			    "an internal path-frame shortage escaped as synchronous ibv_post_send errno %d",
			    post_ret);
	KUNIT_EXPECT_TRUE_MSG(test, pending,
			     "the accepted WR was removed from the verbs SQ");
	KUNIT_EXPECT_TRUE_MSG(test, deferred,
			     "the WR has no state that can retry its initial packetization");
}

static void tbv_initial_send_capacity_preserves_qp_fifo(struct kunit *test)
{
	u32 first_retry_psn = 0;
	u32 second_retry_psn = 0;
	bool first_claimed = false;
	bool second_claimed = true;

	KUNIT_ASSERT_EQ(test,
			tbv_test_initial_send_fifo(&first_claimed,
						   &second_claimed,
						   &first_retry_psn,
						   &second_retry_psn),
			0);
	KUNIT_EXPECT_TRUE_MSG(test, first_claimed,
			     "the QP head did not own its initial post");
	KUNIT_EXPECT_FALSE_MSG(test, second_claimed,
			      "a later WR bypassed a capacity-deferred QP head");
	KUNIT_EXPECT_EQ_MSG(test, first_retry_psn, (u32)101,
			    "the timeout worker did not retry the QP head first");
	KUNIT_EXPECT_EQ_MSG(test, second_retry_psn, (u32)102,
			    "the next WR was not released after the head was fully admitted");
}

static struct kunit_case tbv_send_capacity_defer_cases[] = {
	KUNIT_CASE(tbv_initial_send_capacity_is_deferred),
	KUNIT_CASE(tbv_initial_send_capacity_preserves_qp_fifo),
	{}
};

static struct kunit_suite tbv_send_capacity_defer_suite = {
	.name = "thunderbolt_ibverbs_send_capacity_defer",
	.test_cases = tbv_send_capacity_defer_cases,
};

kunit_test_suite(tbv_send_capacity_defer_suite);
