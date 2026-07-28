// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit: an RNR wait on a remote recv credit must still honour the RNR timer.
 *
 * tbv_send_rnr_waits_for_recv_credit() is recv_credit_required &&
 * rnr_retries_infinite. NCCL sets rnr_retry = 7 (infinite) and every
 * SEND/WRITE_WITH_IMM sets recv_credit_required, so in production this is
 * every send. The reap TX walk skipped such a send outright while no credit
 * was posted, with no timer and no ceiling, leaving the wait dependent solely
 * on tqp->remote_recv_credits. Those credits arrive as unacknowledged
 * single-shot TBV_NATIVE_DATA_OP_RECV_CREDIT frames whose count is incremented
 * before transmission and rolled back only on local enqueue failure, so one
 * frame lost on the wire leaves the two sides permanently asymmetric and the
 * send waiting forever.
 *
 * It is also the wrong contract: rnr_retry = 7 means "retransmit forever,
 * min_rnr_timer apart", not "block on an out-of-band signal".
 *
 * Built into the module on CONFIG_KUNIT; run via tools/run-kunit.sh.
 */
#include <kunit/test.h>
#include "../tbv.h"

/* Far more passes than a walk honouring the RNR timer needs. */
#define TBV_TEST_MAX_PASSES 64U

static void tbv_rnr_credit_wait_honours_timer(struct kunit *test)
{
	u32 passes_used = 0;
	u32 retries = 0;
	u32 credits_left = 0;
	u32 pending = 0;

	/* no credit ever arrives; only the RNR timer can break the wait */
	KUNIT_ASSERT_EQ(test,
			tbv_test_reap_rnr_credit_wait(TBV_TEST_MAX_PASSES, 0,
						      true, &passes_used,
						      &retries, &credits_left,
						      &pending),
			0);

	/* The RNR timer must retransmit as a backstop. */
	KUNIT_EXPECT_EQ(test, retries, 1U);

	/*
	 * Burning the whole budget means every pass skipped the send and left
	 * the state unchanged -- the infinite-wait signature.
	 */
	KUNIT_EXPECT_LT(test, passes_used, TBV_TEST_MAX_PASSES);
}

static void tbv_rnr_credit_wait_wakes_on_credit(struct kunit *test)
{
	u32 passes_used = 0;
	u32 retries = 0;
	u32 credits_left = 0;
	u32 pending = 0;

	/*
	 * A credit is posted and the clock never reaches the RNR timer: the
	 * credit alone must release the send, and consume one credit.
	 */
	KUNIT_ASSERT_EQ(test,
			tbv_test_reap_rnr_credit_wait(TBV_TEST_MAX_PASSES, 1,
						      false, &passes_used,
						      &retries, &credits_left,
						      &pending),
			0);

	KUNIT_EXPECT_EQ(test, retries, 1U);
	KUNIT_EXPECT_EQ(test, passes_used, 1U);
	KUNIT_EXPECT_EQ(test, credits_left, 0U);
}

static struct kunit_case tbv_rnr_credit_wait_cases[] = {
	KUNIT_CASE(tbv_rnr_credit_wait_honours_timer),
	KUNIT_CASE(tbv_rnr_credit_wait_wakes_on_credit),
	{}
};

static struct kunit_suite tbv_rnr_credit_wait_suite = {
	.name = "thunderbolt_ibverbs_rnr_credit_wait",
	.test_cases = tbv_rnr_credit_wait_cases,
};

kunit_test_suite(tbv_rnr_credit_wait_suite);
