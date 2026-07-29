// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit: replay of a captured two-node NCCL hang -- a TX ring that stops
 * completing must not strand its posted frames forever.
 *
 * This suite is not hand-modelled. It replays the state captured on
 * appmana-019/appmana-008 while a two-rank NCCL job hung; the raw captures and
 * the distilled operation sequence are in
 * drivers/thunderbolt_ibverbs/traces/2026-07-28-tx-ring-stall/.
 *
 * What the capture shows, in order:
 *   1. QP brought up normally: RESET->INIT (mask 0x39), INIT->RTR
 *      (mask 0x129181, path_mtu 5, min_rnr_timer 12), RTR->RTS (mask 0x12e01,
 *      timeout 14, retry_cnt 7, rnr_retry 7, max_rd_atomic 1).
 *   2. NCCL posts IBV_WR_RDMA_WRITE; the driver fragments it and hands frames
 *      to the path TX ring. data_tx_posted rises.
 *   3. The ring never completes them: `tx_poll enabled=1 calls=282365
 *      completed=0` on every peer of that rail, with data_tx_completed stuck.
 *      The RX direction is healthy -- the peer's writes arrive and ACKs are
 *      generated -- so the fabric is up; only egress is dead.
 *   4. Nothing ever fails those frames. The queue ceiling
 *      (tbv_path_expire_tx_data_queue) covers tx_data_queue only, and a COPIED
 *      packet -- which is what the fleet uses, data_wr_zcopy=0 in every capture
 *      -- is off that queue the moment it is posted and is not on
 *      tx_zcopy_inflight either. It is referenced only by the ring frame.
 *   5. So the packet's done callback never runs, the owning WR's tx_pending
 *      never drains, and the poll re-arms forever with the path still
 *      advertising active=1 data_ready=1.
 *
 * The WR does eventually die, but only via the per-WR retransmit budget in
 * ibdev.c ("native TX stalled past ceiling, failing WR: ... tx_pending=1"),
 * which surfaces to NCCL as IBV_WC_RETRY_EXC_ERR and kills the job. The path
 * itself is never failed, so the next WR repeats it.
 *
 * The contract asserted here is the same one the credit stall already has: a
 * data packet the driver has taken responsibility for must have a bounded exit.
 *
 * Built into the module on CONFIG_KUNIT; run with tools/run-kunit.sh.
 */
#include <kunit/test.h>
#include "../tbv.h"

/* Generous: 60 passes of 1s against a 5s ceiling is twelve times over. */
#define TBV_STALL_PASSES 60U
#define TBV_STALL_STEP_MS 1000U
#define TBV_STALL_CEILING_MS 5000U

/*
 * The ring really is the thing that stalled. If the poll never ran, or if it
 * completed something, the rest of the suite would be asserting about the
 * wrong scenario -- so pin the captured shape first.
 */
static void tbv_tx_ring_stall_poll_makes_no_progress(struct kunit *test)
{
	u64 poll_completed = 0;
	u64 poll_calls = 0;
	u32 passes_used = 0;

	KUNIT_ASSERT_EQ(test,
			tbv_test_path_tx_ring_stall(TBV_STALL_PASSES,
						    TBV_STALL_STEP_MS,
						    TBV_STALL_CEILING_MS,
						    &passes_used, NULL, NULL,
						    NULL, &poll_calls,
						    &poll_completed, NULL),
			0);

	KUNIT_EXPECT_GT_MSG(test, poll_calls, 0ULL,
			    "the TX poll never ran, so this is not the captured scenario");
	KUNIT_EXPECT_EQ_MSG(test, poll_completed, 0ULL,
			    "the ring completed a frame, so it is not the captured dead ring");
}

/*
 * The hang itself. A frame posted to a ring that never completes has, today,
 * no exit at all: its owner is never told, so the WR behind it never drains.
 */
static void tbv_tx_ring_stall_completes_the_owner(struct kunit *test)
{
	u32 done_calls = 0;
	u32 passes_used = 0;
	u32 inflight = 0;
	int status = 0;

	KUNIT_ASSERT_EQ(test,
			tbv_test_path_tx_ring_stall(TBV_STALL_PASSES,
						    TBV_STALL_STEP_MS,
						    TBV_STALL_CEILING_MS,
						    &passes_used, &inflight,
						    &done_calls, &status, NULL,
						    NULL, NULL),
			0);

	KUNIT_EXPECT_EQ_MSG(test, done_calls, 1U,
			    "owner never completed after %u polls of a dead ring, so its WR tx_pending never drains",
			    passes_used);
	KUNIT_EXPECT_EQ_MSG(test, status, -ETIMEDOUT,
			    "a frame stranded on a dead TX ring must fail as a timeout");
	KUNIT_EXPECT_EQ_MSG(test, inflight, 0U,
			    "tx_inflight never recovered, so the path stays wedged for every later WR");
}

/*
 * Whatever bounds the stall must do it inside the ceiling, not merely
 * eventually: the ceiling is the number the fix is allowed to be judged on.
 */
static void tbv_tx_ring_stall_is_bounded_by_the_ceiling(struct kunit *test)
{
	u32 passes_used = 0;
	u32 done_calls = 0;

	KUNIT_ASSERT_EQ(test,
			tbv_test_path_tx_ring_stall(TBV_STALL_PASSES,
						    TBV_STALL_STEP_MS,
						    TBV_STALL_CEILING_MS,
						    &passes_used, NULL,
						    &done_calls, NULL, NULL,
						    NULL, NULL),
			0);

	KUNIT_EXPECT_LT_MSG(test, passes_used, TBV_STALL_PASSES,
			    "the poll used its whole budget, which is the unbounded re-arm");
}

/*
 * The health gate that already exists (tbv_path_tx_stalled, used to keep new
 * QPs off a dead rail) must actually read true in this state. On the captured
 * node it did not keep NCCL off the rail, and the poll's own stall warning
 * back-dates tx_last_progress_jiffies every time it fires, which is the only
 * clock that gate consults.
 */
static void tbv_tx_ring_stall_is_visible_to_the_health_gate(struct kunit *test)
{
	bool tx_stalled = false;

	KUNIT_ASSERT_EQ(test,
			tbv_test_path_tx_ring_stall(TBV_STALL_PASSES,
						    TBV_STALL_STEP_MS,
						    TBV_STALL_CEILING_MS,
						    NULL, NULL, NULL, NULL,
						    NULL, NULL, &tx_stalled),
			0);

	KUNIT_EXPECT_TRUE_MSG(test, tx_stalled,
			      "a ring with inflight frames and no completions must read as stalled");
}

/* The ceiling stays disablable, exactly as the credit stall's does. */
static void tbv_tx_ring_stall_ceiling_is_disablable(struct kunit *test)
{
	u32 done_calls = 0;

	KUNIT_ASSERT_EQ(test,
			tbv_test_path_tx_ring_stall(4U, TBV_STALL_STEP_MS, 0U,
						    NULL, NULL, &done_calls,
						    NULL, NULL, NULL, NULL),
			0);

	KUNIT_EXPECT_EQ_MSG(test, done_calls, 0U,
			    "timeout_ms=0 must keep the wait-forever contract");
}

static struct kunit_case tbv_tx_ring_stall_cases[] = {
	KUNIT_CASE(tbv_tx_ring_stall_poll_makes_no_progress),
	KUNIT_CASE(tbv_tx_ring_stall_completes_the_owner),
	KUNIT_CASE(tbv_tx_ring_stall_is_bounded_by_the_ceiling),
	KUNIT_CASE(tbv_tx_ring_stall_is_visible_to_the_health_gate),
	KUNIT_CASE(tbv_tx_ring_stall_ceiling_is_disablable),
	{}
};

static struct kunit_suite tbv_tx_ring_stall_suite = {
	.name = "thunderbolt_ibverbs_tx_ring_stall",
	.test_cases = tbv_tx_ring_stall_cases,
};

kunit_test_suite(tbv_tx_ring_stall_suite);
