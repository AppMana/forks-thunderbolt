// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit: a CQ overflow must not fail the QP from inside tbv_cq_push().
 *
 * tbv_cq_push() is reached from inside the RX critical section, which holds
 * tqp->rx_lock: tbv_rx_finish_send(), tbv_rx_deliver_reorder_msg_locked(),
 * tbv_rx_deliver_reorder_write_locked() and tbv_rx_finish_write_locked() all
 * push while locked. Failing the QP there runs tbv_qp_mark_error() ->
 * tbv_qp_flush_error(), which takes tqp->rx_lock again. Linux mutexes are not
 * recursive, so the RX worker blocks forever holding the lock it is waiting
 * for. It is most reachable during an error flush, when
 * tbv_qp_flush_recv_wqes() pushes one WC per outstanding recv WQE and a small
 * CQ overflows partway through.
 *
 * WHAT THIS PROVES: tbv_cq_push() reports the overflow to its caller instead
 * of failing the QP under whatever lock that caller holds, and a caller that
 * has dropped its locks still fails the QP, so no overflow is lost.
 *
 * The lock ordering itself used to be only a structural review property,
 * because a KUnit cannot deadlock-test a real mutex without hanging the run.
 * It is now enforced in code: rx_lock ownership is tracked on every
 * acquisition and the paths that reach tbv_qp_flush_error() refuse to run
 * while the calling task holds it. tbv_rx_lock_reentry_guard_declines below
 * exercises that guard against the real lock, which is safe precisely because
 * the guard means the re-entry no longer blocks.
 *
 * Built into the module on CONFIG_KUNIT; run via tools/run-kunit.sh.
 */
#include <kunit/test.h>
#include "../tbv.h"

static void tbv_cq_overflow_does_not_mark_qp_inline(struct kunit *test)
{
	bool marked_after_drain = false;
	bool marked_in_push = false;
	bool overflow_noted = false;
	int push_ret = 0;
	int first_ret = -1;

	KUNIT_ASSERT_EQ(test,
			tbv_test_cq_overflow_defers_qp_error(&first_ret,
							     &push_ret,
							     &marked_in_push,
							     &overflow_noted,
							     &marked_after_drain),
			0);

	/* the CQ had room for exactly one WC */
	KUNIT_EXPECT_EQ(test, first_ret, 0);

	/* overflow is reported to the caller */
	KUNIT_EXPECT_EQ(test, push_ret, -ENOSPC);

	/*
	 * The push itself must not have failed the QP: doing so from a caller
	 * holding rx_lock is the self-deadlock.
	 */
	KUNIT_EXPECT_FALSE(test, marked_in_push);

	/* but the overflow must be recorded, not dropped */
	KUNIT_EXPECT_TRUE(test, overflow_noted);

	/* and a caller outside the locked section must fail the QP */
	KUNIT_EXPECT_TRUE(test, marked_after_drain);
}

/*
 * A caller that holds rx_lock must be refused, not blocked, and a caller that
 * does not must be unaffected. Without the guard the first half of this is a
 * deadlock rather than a failed expectation, which is why the deadlock could
 * only ever be argued structurally before.
 */
static void tbv_rx_lock_reentry_guard_declines(struct kunit *test)
{
	bool violation = false;
	bool marked = false;

	KUNIT_ASSERT_EQ(test,
			tbv_test_rx_lock_reentry_guard(true, &violation,
						       &marked),
			0);
	KUNIT_EXPECT_TRUE_MSG(test, violation,
			      "guard did not see rx_lock held by the calling task");
	KUNIT_EXPECT_FALSE_MSG(test, marked,
			       "QP error path ran under rx_lock instead of declining");

	violation = true;
	marked = false;
	KUNIT_ASSERT_EQ(test,
			tbv_test_rx_lock_reentry_guard(false, &violation,
						       &marked),
			0);
	KUNIT_EXPECT_FALSE_MSG(test, violation,
			       "guard reported a violation with rx_lock free");
	KUNIT_EXPECT_TRUE_MSG(test, marked,
			      "QP error path declined with rx_lock free");
}

static struct kunit_case tbv_cq_overflow_deadlock_cases[] = {
	KUNIT_CASE(tbv_cq_overflow_does_not_mark_qp_inline),
	KUNIT_CASE(tbv_rx_lock_reentry_guard_declines),
	{}
};

static struct kunit_suite tbv_cq_overflow_deadlock_suite = {
	.name = "thunderbolt_ibverbs_cq_overflow_deadlock",
	.test_cases = tbv_cq_overflow_deadlock_cases,
};

kunit_test_suite(tbv_cq_overflow_deadlock_suite);
