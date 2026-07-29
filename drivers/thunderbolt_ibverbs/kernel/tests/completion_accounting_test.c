// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit: every error the consumer sees on the send CQ must also be visible in
 * the driver's counters.
 *
 * The chain reproducer produced a state nobody could act on: perftest died
 * with "Failed status 11" (IBV_WC_RETRY_EXC_ERR) while the same node's
 * debugfs summary read data_wr_retry_exhausted 0 and data_wr_timeout 0. Two
 * views of one WR, and neither could be trusted over the other.
 *
 * The cause is where the counting lives. tbv_qp_timeout_work() counted
 * -ETIMEDOUT and -EAGAIN in its own completed_sends loop, which only ever
 * contains the sends the ready-prefix drain managed to move. A send the reap
 * walk marks ready behind a still-pending earlier entry stays on
 * pending_sends; it is delivered later by tbv_qp_destroy() or
 * tbv_qp_flush_error(), both of which copy completion_status verbatim and
 * increment nothing. The application gets RETRY_EXC_ERR, the counters stay at
 * zero, and the two disagree.
 *
 * These pin the invariant at the funnel point instead: one WC in error, one
 * count, whichever path delivered it.
 *
 * Built into the module on CONFIG_KUNIT; run via tools/run-kunit.sh.
 */
#include <kunit/test.h>
#include <rdma/ib_verbs.h>
#include "../tbv.h"

static void tbv_flushed_timeout_is_counted(struct kunit *test)
{
	u64 timeouts = 0;
	u64 rnr = 0;
	int wc_status = 0;

	KUNIT_ASSERT_EQ(test,
			tbv_test_send_complete_accounting(-ETIMEDOUT,
							  &wc_status,
							  &timeouts, &rnr),
			0);
	/* what the application polls */
	KUNIT_EXPECT_EQ(test, wc_status, (int)IB_WC_RETRY_EXC_ERR);
	/* what the operator reads back; zero here is the whole bug */
	KUNIT_EXPECT_EQ(test, timeouts, 1ULL);
	KUNIT_EXPECT_EQ(test, rnr, 0ULL);
}

static void tbv_flushed_rnr_exhaustion_is_counted(struct kunit *test)
{
	u64 timeouts = 0;
	u64 rnr = 0;
	int wc_status = 0;

	KUNIT_ASSERT_EQ(test,
			tbv_test_send_complete_accounting(-EAGAIN, &wc_status,
							  &timeouts, &rnr),
			0);
	KUNIT_EXPECT_EQ(test, wc_status, (int)IB_WC_RNR_RETRY_EXC_ERR);
	KUNIT_EXPECT_EQ(test, rnr, 1ULL);
	KUNIT_EXPECT_EQ(test, timeouts, 0ULL);
}

static void tbv_flush_does_not_count_as_timeout(struct kunit *test)
{
	u64 timeouts = 0;
	u64 rnr = 0;
	int wc_status = 0;

	/*
	 * The counter must not become a catch-all either: an ordinary flush is
	 * WR_FLUSH_ERR and is neither a transport timeout nor RNR exhaustion.
	 */
	KUNIT_ASSERT_EQ(test,
			tbv_test_send_complete_accounting(-ECANCELED,
							  &wc_status,
							  &timeouts, &rnr),
			0);
	KUNIT_EXPECT_EQ(test, wc_status, (int)IB_WC_WR_FLUSH_ERR);
	KUNIT_EXPECT_EQ(test, timeouts, 0ULL);
	KUNIT_EXPECT_EQ(test, rnr, 0ULL);
}

static struct kunit_case tbv_completion_accounting_cases[] = {
	KUNIT_CASE(tbv_flushed_timeout_is_counted),
	KUNIT_CASE(tbv_flushed_rnr_exhaustion_is_counted),
	KUNIT_CASE(tbv_flush_does_not_count_as_timeout),
	{}
};

static struct kunit_suite tbv_completion_accounting_suite = {
	.name = "thunderbolt_ibverbs_completion_accounting",
	.test_cases = tbv_completion_accounting_cases,
};

kunit_test_suite(tbv_completion_accounting_suite);
