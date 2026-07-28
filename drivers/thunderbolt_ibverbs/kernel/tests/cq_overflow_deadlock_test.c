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
 * WHAT THIS DOES NOT PROVE: that no remaining code path calls
 * tbv_qp_mark_error() while holding tqp->rx_lock. A KUnit cannot deadlock-test
 * a real mutex without hanging the whole run, so the lock ordering itself
 * stays a structural review property; this pins the one push site that
 * violated it.
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

static struct kunit_case tbv_cq_overflow_deadlock_cases[] = {
	KUNIT_CASE(tbv_cq_overflow_does_not_mark_qp_inline),
	{}
};

static struct kunit_suite tbv_cq_overflow_deadlock_suite = {
	.name = "thunderbolt_ibverbs_cq_overflow_deadlock",
	.test_cases = tbv_cq_overflow_deadlock_cases,
};

kunit_test_suite(tbv_cq_overflow_deadlock_suite);
