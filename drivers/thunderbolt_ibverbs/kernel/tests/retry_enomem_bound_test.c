// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit: a retransmit the local path TX queue keeps rejecting must converge.
 *
 * tbv_path_reserve_data() returns -ENOMEM whenever the path TX queue is full,
 * and the queue drains only as the peer's software credit window advances, so
 * queue-full is a peer-controlled condition that can persist indefinitely. The
 * timeout work's -ENOMEM arm re-stamps the send's deadline and re-arms without
 * charging any budget, so retries never reaches max_retries, the send never
 * exhausts and the WR neither completes nor fails.
 *
 * The requirement has two sides: the wait must be BOUNDED (the send converges
 * to a completion), and a short queue-full burst must not fail the WR
 * outright, so several posts must be tolerated before the budget is charged.
 *
 * Built into the module on CONFIG_KUNIT; run via tools/run-kunit.sh.
 */
#include <kunit/test.h>
#include "../tbv.h"

/* Far more cycles than any converging retransmit loop needs. */
#define TBV_TEST_MAX_PASSES 512U

/* A transient queue-full must survive at least this many posts uncharged. */
#define TBV_TEST_MIN_TOLERATED_POSTS 4U

static void tbv_retry_enomem_is_bounded(struct kunit *test)
{
	u32 passes_used = 0;
	u32 posts = 0;
	u32 retries = 0;
	bool failed = false;

	KUNIT_ASSERT_EQ(test,
			tbv_test_retry_enomem_bounded(TBV_TEST_MAX_PASSES,
						      &passes_used, &posts,
						      &failed, &retries),
			0);

	/*
	 * Burning the whole budget means every cycle re-armed the same send on
	 * unchanged state -- the unbounded-retransmit signature.
	 */
	KUNIT_EXPECT_LT(test, passes_used, TBV_TEST_MAX_PASSES);

	/* The send must end up completed, not pending forever. */
	KUNIT_EXPECT_TRUE(test, failed);

	/* Persistent queue-full must spend the WR retry budget. */
	KUNIT_EXPECT_GE(test, retries, 1U);

	/* But a brief queue-full burst must not fail the WR immediately. */
	KUNIT_EXPECT_GE(test, posts, TBV_TEST_MIN_TOLERATED_POSTS);
}

static struct kunit_case tbv_retry_enomem_bound_cases[] = {
	KUNIT_CASE(tbv_retry_enomem_is_bounded),
	{}
};

static struct kunit_suite tbv_retry_enomem_bound_suite = {
	.name = "thunderbolt_ibverbs_retry_enomem_bound",
	.test_cases = tbv_retry_enomem_bound_cases,
};

kunit_test_suite(tbv_retry_enomem_bound_suite);
