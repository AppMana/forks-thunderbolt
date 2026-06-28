// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit: path-layer data-credit ping-pong (deterministic model of the 16K hang).
 *
 * ib_send_lat at 16384 bytes hangs; at 64 bytes it does not. The difference is
 * frame count: a 16K message is 5 native data frames (DIV_ROUND_UP(16384, 4048),
 * MAX_PAYLOAD = 4096 - 48), a 64-byte message is 1. The path layer gates each
 * data frame on a software credit window (path.c:tbv_path_schedule_tx) and the
 * receiver RETURNS credits batched at tbv_native_data_credit_return_threshold().
 * A multi-frame send must acquire min(frames, threshold) credits to START its
 * group (tbv_native_data_start_credit_required), and in a strict 1-in-flight
 * ping-pong the receiver's sub-threshold remainder is never flushed -- so the
 * peer can WITHHOLD up to (threshold - 1) credits indefinitely.
 *
 * The deadlock-free invariant is therefore:
 *     window - (threshold - 1) >= min(frames_per_msg, threshold)
 * i.e. the credits the peer can never legally withhold must still cover one
 * message's start requirement. The old threshold min(window, BATCH) returns
 * `window` for window < BATCH, letting the peer withhold window-1 and strand any
 * multi-frame sender (window 5..35, frames=5: dead). A single-frame sender
 * (frames=1) only ever needs 1 credit and is immune -- exactly why 64B works and
 * 16K hangs.
 *
 * This walks the real path.c credit ledger using the real header helpers and
 * pins: (a) a single-frame ping-pong always progresses (the 64B case), and
 * (b) a multi-frame ping-pong progresses at EVERY window that can physically
 * hold the message (the 16K case "shouldn't hang"). Built on CONFIG_KUNIT; run
 * via tools/run-kunit.sh.
 */
#include <kunit/test.h>
#include "../../proto/native_data.h"

/*
 * Faithful replay of one direction of a strict 1-in-flight ping-pong against the
 * real start-gate (path.c:1850-1867) and the real batched return
 * (path.c:tbv_path_return_rx_data_credit). Returns the round it deadlocked on,
 * or -1 if it ran @rounds rounds without stalling. The model is charitable to
 * the current code: the return batch is applied instantly as the receiver
 * consumes (no transit delay), so any stall it reports is a true policy
 * deadlock, not a timing artifact.
 */
static int credit_pingpong_stall_round(u32 window, u32 frames_per_msg,
				       u32 rounds)
{
	u32 threshold = tbv_native_data_credit_return_threshold(window);
	u32 credits = window;	/* tbv_path_set_remote_rx_capacity seeds = window */
	u32 pending = 0;	/* receiver's rx_data_credit_pending */
	u32 r;
	u32 f;

	for (r = 0; r < rounds; r++) {
		/* Start gate: first frame of the group needs the start credit. */
		u32 start_required = tbv_native_data_start_credit_required(
			frames_per_msg, window);

		if (start_required < 1)
			start_required = 1;
		if (credits < start_required)
			return r;

		/* Send each frame: consume 1, receiver consumes 1 and batches. */
		for (f = 0; f < frames_per_msg; f++) {
			if (credits < 1)
				return r;
			credits--;
			pending++;
			if (pending >= threshold) {
				credits += pending;
				pending = 0;
			}
		}
		/* Quiescent: receiver holds pending < threshold, never flushed. */
	}
	return -1;
}

/* The 64-byte case: a single-frame ping-pong must never stall, any window. */
static void tbv_credit_single_frame_never_stalls(struct kunit *test)
{
	u32 windows[] = { 4, 6, 8, 16, 35, 36, 64, 768 };
	int i;

	for (i = 0; i < (int)ARRAY_SIZE(windows); i++)
		KUNIT_EXPECT_EQ_MSG(test,
			credit_pingpong_stall_round(windows[i], 1, 200), -1,
			"single-frame ping-pong stalled at window=%u",
			windows[i]);
}

/*
 * The 16K case: a 5-frame ping-pong must never stall at any window that can
 * physically hold the message (frames <= window). This FAILS against the old
 * min(window, BATCH) threshold for windows 5..35 (the multi-frame deadlock) and
 * passes once the threshold is bounded so the peer cannot withhold a message's
 * worth of credits.
 */
static void tbv_credit_multi_frame_never_stalls(struct kunit *test)
{
	u32 frames = 5;	/* 16384 bytes / 4048 payload, rounded up */
	u32 window;

	for (window = frames; window <= 4096; window++)
		KUNIT_EXPECT_EQ_MSG(test,
			credit_pingpong_stall_round(window, frames, 400), -1,
			"16K (5-frame) ping-pong stalled at window=%u", window);
}

/*
 * The invariant directly: the credits the peer can NEVER withhold
 * (window - (threshold - 1)) must cover one message's start requirement
 * (min(frames, threshold)) for every window that can hold the message.
 */
static void tbv_credit_return_threshold_invariant(struct kunit *test)
{
	u32 window;

	for (window = 1; window <= 4096; window++) {
		u32 threshold =
			tbv_native_data_credit_return_threshold(window);
		u32 always_available = window - (threshold - 1);
		u32 frames = window;	/* largest message the window can hold */
		u32 start_required =
			tbv_native_data_start_credit_required(frames, window);

		KUNIT_EXPECT_GE_MSG(test, always_available, start_required,
			"window=%u: peer may withhold %u, stranding a %u-credit start",
			window, threshold - 1, start_required);
	}
}

/*
 * Replay a lossy multi-frame ping-pong with full-message retransmit. A data
 * frame charges a credit when it leaves; the peer returns it only when RECEIVED.
 * A frame lost in transit is charged but never returned, and the retransmit
 * re-charges it -- a per-loss leak. @refund applies tbv_native_data_refund_credits
 * (the real fix) before each retransmit. Deterministic loss: every @loss_period-th
 * frame is dropped on its FIRST send (retransmits always deliver). Returns the
 * round it deadlocked on, or -1 if it survived @rounds.
 */
static int credit_lossy_pingpong_stall_round(u32 window, u32 frames_per_msg,
					     u32 loss_period, bool refund,
					     u32 rounds)
{
	u32 threshold = tbv_native_data_credit_return_threshold(window);
	u32 credits = window;
	u32 pending = 0;
	u64 frame_seq = 0;	/* global frame counter for the loss pattern */
	u32 r;

	for (r = 0; r < rounds; r++) {
		u32 attempt = 0;
		bool delivered = false;

		while (!delivered) {
			u32 start_required =
				tbv_native_data_start_credit_required(
					frames_per_msg, window);
			bool any_lost = false;
			u32 f;

			if (start_required < 1)
				start_required = 1;
			if (credits < start_required)
				return r;

			for (f = 0; f < frames_per_msg; f++) {
				bool lost;

				if (credits < 1)
					return r;
				credits--;
				/* first-send loss only; retransmits deliver */
				lost = attempt == 0 && loss_period &&
				       (frame_seq % loss_period) == 0;
				frame_seq++;
				if (lost) {
					any_lost = true;
					continue;
				}
				pending++;
				if (pending >= threshold) {
					credits += pending;
					pending = 0;
				}
			}

			if (any_lost) {
				if (refund)
					credits = tbv_native_data_refund_credits(
						credits, window, frames_per_msg);
				attempt++;
				continue;
			}
			delivered = true;
		}
	}
	return -1;
}

/*
 * The leak: under loss, a 5-frame ping-pong at the default window WITHOUT the
 * refund drains to a deadlock (documents the bug); WITH the refund it survives
 * indefinitely (the fix). 768 window, ~1.5% loss (1/64 frames).
 */
static void tbv_credit_lossy_refund_stops_leak(struct kunit *test)
{
	u32 window = 768;	/* default native_ring_size 1024 - 256 reserve */
	u32 frames = 5;		/* 16K message */
	u32 loss_period = 64;	/* ~1.5% first-send frame loss */

	/* Without the refund the leak eventually deadlocks the sender. */
	KUNIT_EXPECT_GE_MSG(test,
		credit_lossy_pingpong_stall_round(window, frames, loss_period,
						  false, 100000), 0,
		"no-refund lossy ping-pong unexpectedly survived (leak absent?)");
	/* With the refund the sender never stalls. */
	KUNIT_EXPECT_EQ_MSG(test,
		credit_lossy_pingpong_stall_round(window, frames, loss_period,
						  true, 100000), -1,
		"refund failed to stop the per-loss credit leak");
}

static struct kunit_case tbv_credit_pingpong_test_cases[] = {
	KUNIT_CASE(tbv_credit_single_frame_never_stalls),
	KUNIT_CASE(tbv_credit_multi_frame_never_stalls),
	KUNIT_CASE(tbv_credit_return_threshold_invariant),
	KUNIT_CASE(tbv_credit_lossy_refund_stops_leak),
	{}
};

static struct kunit_suite tbv_credit_pingpong_test_suite = {
	.name = "thunderbolt_ibverbs_credit_pingpong",
	.test_cases = tbv_credit_pingpong_test_cases,
};
kunit_test_suite(tbv_credit_pingpong_test_suite);
