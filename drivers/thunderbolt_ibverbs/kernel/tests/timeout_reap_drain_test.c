// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit: the QP timeout reap's TX walk must survive an exhausted head send
 * with ready out-of-order-acked entries queued behind it.
 *
 * Hardware (appmana-019, pstore 2026-07-23, tbv on 6.17.0-40): a WR retry
 * exhausted warn was followed within 8us by two one-shot UBSAN invalid-bool
 * loads in tbv_qp_timeout_work.cold and 19s later by an NMI hard-lockup
 * panic at the walk's send->ready load, EFLAGS.IF clear. Root cause: the
 * exhausted branches drained the ready prefix of pending_sends mid-walk;
 * out-of-order ACKs leave later entries ready behind a pending head, so the
 * drain could move the list_for_each_entry_safe prefetched cursor onto the
 * on-stack completed list and the walk never reached the pending_sends head
 * sentinel again -- an infinite loop under the irq-off QP lock.
 *
 * This test rebuilds exactly that list state and runs the reap. On the
 * pre-fix code it never returns (RED via the kunit.py timeout); on the fixed
 * code the walk terminates and the whole ready prefix completes.
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
