// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit: the QP timeout reap's TX walk must survive an exhausted head send
 * with ready out-of-order-acked entries queued behind it.
 *
 * Out-of-order ACKs leave later entries ready behind a still-pending head,
 * so the ready-prefix drain triggered by the head exhausting its retry
 * budget can cover the walk's prefetched list_for_each_entry_safe cursor.
 * The walk must terminate from this state, complete the whole ready prefix
 * and leave pending_sends empty. See the fix commit for the failure this
 * pins.
 *
 * Built into the module on CONFIG_KUNIT; run via tools/run-kunit.sh.
 */
#include <kunit/test.h>
#include "../tbv.h"

static void tbv_reap_tx_survives_drain_past_cursor(struct kunit *test)
{
	u32 completed = 0;
	u32 pending = 0;
	bool tx_failed = false;

	KUNIT_ASSERT_EQ(test,
			tbv_test_reap_tx_exhausted_head_ready_tail(
				&completed, &pending, &tx_failed),
			0);
	/* the exhausted head and both ready entries behind it complete */
	KUNIT_EXPECT_EQ(test, completed, 3U);
	/* nothing is left behind on pending_sends */
	KUNIT_EXPECT_EQ(test, pending, 0U);
	/* retry exhaustion still reports TX failure to the caller */
	KUNIT_EXPECT_TRUE(test, tx_failed);
}

static struct kunit_case tbv_timeout_reap_drain_cases[] = {
	KUNIT_CASE(tbv_reap_tx_survives_drain_past_cursor),
	{}
};

static struct kunit_suite tbv_timeout_reap_drain_suite = {
	.name = "thunderbolt_ibverbs_timeout_reap_drain",
	.test_cases = tbv_timeout_reap_drain_cases,
};

kunit_test_suite(tbv_timeout_reap_drain_suite);
