// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit: a send whose tx_pending never clears must not defer forever.
 *
 * The TX walk skips any send with frames still posting so a retransmit cannot
 * race the posting path. That skip happens before the retry and exhaustion
 * branches, so a frame the fabric never drains leaves its send unable to
 * retry, unable to exhaust and unable to complete, while the walk keeps
 * reporting that another pass is needed. The QP never surfaces an error and
 * the timeout work re-arms on the same unchanged state indefinitely.
 *
 * Ready out-of-order-acked entries queued behind such a head cannot drain
 * either, because the drain only moves the ready prefix.
 *
 * Built into the module on CONFIG_KUNIT; run via tools/run-kunit.sh.
 */
#include <kunit/test.h>
#include "../tbv.h"

/* Far more passes than any converging walk needs. */
#define TBV_TEST_MAX_PASSES 64U

static void tbv_reap_tx_converges_on_stalled_head(struct kunit *test)
{
	u32 passes_used = 0;
	u32 completed = 0;
	u32 pending = 0;
	bool tx_failed = false;

	KUNIT_ASSERT_EQ(test,
			tbv_test_reap_tx_stalled_tx_pending_head(
				TBV_TEST_MAX_PASSES, &passes_used, &completed,
				&pending, &tx_failed),
			0);

	/*
	 * Burning the whole budget means every pass left the state unchanged
	 * and asked for another -- the live-wedge signature.
	 */
	KUNIT_EXPECT_LT(test, passes_used, TBV_TEST_MAX_PASSES);

	/* The stalled head and the ready tail behind it must all complete. */
	KUNIT_EXPECT_EQ(test, completed, 3U);
	KUNIT_EXPECT_EQ(test, pending, 0U);

	/* Failing a stalled send is a TX failure the caller must see. */
	KUNIT_EXPECT_TRUE(test, tx_failed);
}

static struct kunit_case tbv_tx_stall_livelock_cases[] = {
	KUNIT_CASE(tbv_reap_tx_converges_on_stalled_head),
	{}
};

static struct kunit_suite tbv_tx_stall_livelock_suite = {
	.name = "thunderbolt_ibverbs_tx_stall_livelock",
	.test_cases = tbv_tx_stall_livelock_cases,
};

kunit_test_suite(tbv_tx_stall_livelock_suite);
