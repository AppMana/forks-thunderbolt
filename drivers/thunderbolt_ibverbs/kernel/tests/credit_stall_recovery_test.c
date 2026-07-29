// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit: a peer that stops returning data credits must not strand queued
 * packets forever.
 *
 * Every native data packet is admitted by a software credit window
 * (tbv_path_schedule_tx) that only advances when the peer sends a PATH_CREDIT
 * frame. Those frames are unacknowledged and single-shot, so a rail flap, a
 * wedged peer QP or one lost credit frame leaves the window short with nothing
 * to refill it. The packets stay on tx_data_queue, their done callbacks never
 * run, and the owning WR's tx_pending never drains.
 *
 * The scenario runs against the real path code: real tbv_path_send(), the real
 * credit gate (data_tx_credit_stalls confirms the gate is what stopped the
 * packet, not a staging-frame shortage), and the real TX work. See the hook in
 * path.c for which driver paths set each field it initialises.
 *
 * Built into the module on CONFIG_KUNIT; run via tools/run-kunit.sh.
 */
#include <kunit/test.h>
#include "../tbv.h"

static void tbv_credit_stall_queue_has_a_ceiling(struct kunit *test)
{
	u32 done_calls = 0;
	u32 stalls = 0;
	u32 queued = 0;
	int status = 0;

	/* Queued 30s ago against a 1s ceiling: unambiguously overdue. */
	KUNIT_ASSERT_EQ(test,
			tbv_test_path_credit_stall_timeout(30000, 1000, &queued,
							   &stalls, &done_calls,
							   &status),
			0);

	/* The credit gate, not a frame shortage, is what held the packet. */
	KUNIT_EXPECT_GT(test, stalls, 0U);

	KUNIT_EXPECT_EQ_MSG(test, queued, 0U,
			    "packet still queued past the ceiling with no credits coming");
	KUNIT_EXPECT_EQ_MSG(test, done_calls, 1U,
			    "owner never completed, so its tx_pending never drains");
	KUNIT_EXPECT_EQ_MSG(test, status, -ETIMEDOUT,
			    "a credit-starved packet must fail as a timeout");
}

/* Inside the ceiling the packet must keep waiting: credits may still arrive. */
static void tbv_credit_stall_waits_inside_the_ceiling(struct kunit *test)
{
	u32 done_calls = 0;
	u32 stalls = 0;
	u32 queued = 0;
	int status = 0;

	KUNIT_ASSERT_EQ(test,
			tbv_test_path_credit_stall_timeout(0, 30000, &queued,
							   &stalls, &done_calls,
							   &status),
			0);

	KUNIT_EXPECT_GT(test, stalls, 0U);
	KUNIT_EXPECT_EQ(test, queued, 1U);
	KUNIT_EXPECT_EQ(test, done_calls, 0U);
}

/* tx_queue_timeout_ms = 0 keeps the old wait-forever contract available. */
static void tbv_credit_stall_ceiling_is_disablable(struct kunit *test)
{
	u32 done_calls = 0;
	u32 stalls = 0;
	u32 queued = 0;
	int status = 0;

	KUNIT_ASSERT_EQ(test,
			tbv_test_path_credit_stall_timeout(30000, 0, &queued,
							   &stalls, &done_calls,
							   &status),
			0);

	KUNIT_EXPECT_EQ(test, queued, 1U);
	KUNIT_EXPECT_EQ(test, done_calls, 0U);
}

static struct kunit_case tbv_credit_stall_recovery_cases[] = {
	KUNIT_CASE(tbv_credit_stall_queue_has_a_ceiling),
	KUNIT_CASE(tbv_credit_stall_waits_inside_the_ceiling),
	KUNIT_CASE(tbv_credit_stall_ceiling_is_disablable),
	{}
};

static struct kunit_suite tbv_credit_stall_recovery_suite = {
	.name = "thunderbolt_ibverbs_credit_stall_recovery",
	.test_cases = tbv_credit_stall_recovery_cases,
};

kunit_test_suite(tbv_credit_stall_recovery_suite);
