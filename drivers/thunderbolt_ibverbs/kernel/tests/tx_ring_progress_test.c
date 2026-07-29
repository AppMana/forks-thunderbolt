// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit: what counts as TX ring progress, and who is allowed to observe it.
 *
 * Replays the shape captured in
 * drivers/thunderbolt_ibverbs/traces/baseline-025-023/hang-appmana-023.txt,
 * where the driver reports
 *
 *   [422977.005598] tx stall route=0x3 rail=0x0 inflight=1 posted=10876 completed=10875
 *   [422982.125686] tx stall route=0x3 rail=0x0 inflight=1 posted=10881 completed=10880
 *   [422987.246724] tx stall route=0x3 rail=0x0 inflight=1 posted=10886 completed=10885
 *
 * every 5.1 s on two paths at once. Both posted AND completed advance by five
 * between consecutive lines, so no frame is stranded: the ring is completing
 * everything it is given. What the path cannot see is those completions,
 * because the TX ring is interrupt-driven (NHI MSI-X -> ring_work ->
 * frame->callback) and only the supplemental poll used to stamp progress.
 *
 * That stamp is not cosmetic. tbv_path_tx_stalled() reads it at a 1.5 s
 * threshold and ibdev.c skips any rail it calls stalled when binding a new QP,
 * so a busy healthy rail whose completions all arrive by interrupt is
 * unbindable for most of every five second window.
 *
 * Built into the module on CONFIG_KUNIT; run with tools/run-kunit.sh.
 */
#include <kunit/test.h>
#include "../tbv.h"

/* Past tx_stall_skip_ms (1500) and tx_stall_warn_ms (5000) both. */
#define TBV_PROGRESS_STALL_MS 6000U
#define TBV_PROGRESS_PASSES 4U

/*
 * The captured scenario, pinned: the poll itself reaps nothing (the interrupt
 * path got there first) and a frame really is outstanding at sample time.
 */
static void tbv_tx_progress_poll_reaps_nothing(struct kunit *test)
{
	u64 poll_completed = 1;
	u32 inflight = 0;

	KUNIT_ASSERT_EQ(test,
			tbv_test_path_tx_interrupt_progress(
				TBV_PROGRESS_PASSES, TBV_PROGRESS_STALL_MS,
				NULL, &poll_completed, &inflight),
			0);

	KUNIT_EXPECT_EQ_MSG(test, poll_completed, 0ULL,
			    "the poll reaped a frame, so this is not the captured interrupt-drained ring");
	KUNIT_EXPECT_EQ_MSG(test, inflight, 1U,
			    "one frame must still be outstanding, as in posted=N completed=N-1");
}

/*
 * The bug. A completion delivered by the interrupt path is progress; a path
 * that just completed a frame must not read as stalled to the rail health
 * gate, or new QPs are refused a rail that is working.
 */
static void tbv_tx_progress_interrupt_completion_counts(struct kunit *test)
{
	bool tx_stalled = true;

	KUNIT_ASSERT_EQ(test,
			tbv_test_path_tx_interrupt_progress(
				TBV_PROGRESS_PASSES, TBV_PROGRESS_STALL_MS,
				&tx_stalled, NULL, NULL),
			0);

	KUNIT_EXPECT_FALSE_MSG(test, tx_stalled,
			       "a ring that completed a frame through the interrupt path reads as stalled, so ibdev skips this rail for every new QP");
}

/* Ceiling for the released-frame test; the hook ages the frame past it. */
#define TBV_PROGRESS_CEILING_MS 1000U

/*
 * The ring-frame ceiling hands the inflight slot back when it gives up on a
 * frame. The ring still owns that frame's buffer and completes it later --
 * canceled at worst, on teardown. Both events must not spend the same slot.
 */
static void tbv_tx_progress_expired_frame_completes_once(struct kunit *test)
{
	u32 done_calls = 0;
	int inflight = 1;
	int status = 0;

	KUNIT_ASSERT_EQ(test,
			tbv_test_path_tx_expired_frame_completes(
				TBV_PROGRESS_CEILING_MS, &inflight, &done_calls,
				&status),
			0);

	KUNIT_EXPECT_EQ_MSG(test, done_calls, 1U,
			    "the owner must be completed exactly once by the ceiling");
	KUNIT_EXPECT_EQ_MSG(test, status, -ETIMEDOUT,
			    "the ceiling fails a stranded frame as a timeout");
	KUNIT_EXPECT_EQ_MSG(test, inflight, 0,
			    "tx_inflight is %d after a released frame completed late; tbv_path_schedule_tx() reads that into a u32 and then refuses every staged data frame for the life of the path",
			    inflight);
}

/*
 * The other half of the same wedge, seen live on appmana-025 route=0x3:
 * tx_credits=768/768, nothing inflight, data_tx_posted equal to
 * data_tx_completed, and 66269 packets queued that the path will never post.
 * Every gate in tbv_path_schedule_tx() but one is open in that state; the one
 * that is shut is the unframed window, held open by an owner whose remaining
 * packets a (done, done_ctx) cancel took away.
 */
static void tbv_tx_progress_cancel_closes_raw_window(struct kunit *test)
{
	bool window_open = true;
	u32 done_calls = 0;
	u32 queued = 0;

	KUNIT_ASSERT_EQ(test,
			tbv_test_path_cancel_orphans_raw_window(&window_open,
								&queued,
								&done_calls),
			0);

	KUNIT_EXPECT_EQ_MSG(test, done_calls, 1U,
			    "the cancelled owner must be completed");
	KUNIT_EXPECT_EQ_MSG(test, queued, 1U,
			    "the other owner's packet must still be queued and sendable");
	KUNIT_EXPECT_FALSE_MSG(test, window_open,
			       "the unframed window is still open on an owner with nothing left to send, so tbv_path_schedule_tx() refuses the queue head forever");
}

static struct kunit_case tbv_tx_ring_progress_cases[] = {
	KUNIT_CASE(tbv_tx_progress_poll_reaps_nothing),
	KUNIT_CASE(tbv_tx_progress_interrupt_completion_counts),
	KUNIT_CASE(tbv_tx_progress_expired_frame_completes_once),
	KUNIT_CASE(tbv_tx_progress_cancel_closes_raw_window),
	{}
};

static struct kunit_suite tbv_tx_ring_progress_suite = {
	.name = "thunderbolt_ibverbs_tx_ring_progress",
	.test_cases = tbv_tx_ring_progress_cases,
};

kunit_test_suite(tbv_tx_ring_progress_suite);
