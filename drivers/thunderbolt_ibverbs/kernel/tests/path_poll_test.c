// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit: the poll-mode ring-completion contract.
 *
 * Converting the tbv data rings off the shared-system-workqueue schedule_work
 * completion path onto the NHI poll API (start_poll -> tasklet) requires the
 * tasklet to DRAIN the ring to empty before RE-ARMING the interrupt: re-arm
 * early and a completion that lands between the last poll and the unmask is
 * lost until the next post (a stall / the latency tail we are trying to kill);
 * never re-arm and the ring wedges. This pins tbv_path_poll_complete_ring()'s
 * invariant: every completed frame's callback fires, then rearm() is called
 * exactly once, AFTER the last poll.
 *
 * Built into the module on CONFIG_KUNIT; run via tools/run-kunit.sh.
 */
#include <kunit/test.h>
#include <linux/string.h>
#include <linux/thunderbolt.h>
#include "../tbv.h"

#define POLL_TEST_FRAMES 5

struct poll_test_ctx {
	struct ring_frame frames[POLL_TEST_FRAMES];
	int n;		/* frames to hand out */
	int next;	/* next index poll() returns */
	int tick;	/* monotonic order stamp */
	int cb_count;	/* callbacks invoked */
	int last_cb_tick;
	int rearm_count;
	int rearm_tick;	/* -1 until rearm fires */
};

/* Tests are serialized; one static context is safe and is reset per test. */
static struct poll_test_ctx PTC;

static void poll_test_frame_cb(struct tb_ring *ring, struct ring_frame *frame,
			       bool canceled)
{
	PTC.cb_count++;
	PTC.last_cb_tick = PTC.tick++;
}

static struct ring_frame *poll_test_poll(struct tb_ring *ring)
{
	if (PTC.next >= PTC.n)
		return NULL;
	return &PTC.frames[PTC.next++];
}

static void poll_test_rearm(struct tb_ring *ring)
{
	PTC.rearm_count++;
	PTC.rearm_tick = PTC.tick++;
}

static void poll_test_reset(int n)
{
	int i;

	memset(&PTC, 0, sizeof(PTC));
	PTC.n = n;
	PTC.rearm_tick = -1;
	PTC.last_cb_tick = -1;
	for (i = 0; i < n; i++)
		PTC.frames[i].callback = poll_test_frame_cb;
}

/* N frames: every callback fires, rearm once, AFTER the last drained frame. */
static void tbv_path_poll_drains_then_rearms(struct kunit *test)
{
	u64 drained;

	poll_test_reset(POLL_TEST_FRAMES);
	drained = tbv_path_poll_complete_ring(NULL, poll_test_poll,
					      poll_test_rearm);

	KUNIT_EXPECT_EQ(test, drained, (u64)POLL_TEST_FRAMES);
	KUNIT_EXPECT_EQ(test, PTC.cb_count, POLL_TEST_FRAMES);
	KUNIT_EXPECT_EQ(test, PTC.rearm_count, 1);
	/* load-bearing: re-arm must happen AFTER the last drained frame */
	KUNIT_EXPECT_GT(test, PTC.rearm_tick, PTC.last_cb_tick);
}

/* Empty ring: no callbacks, but still re-arm exactly once. */
static void tbv_path_poll_empty_still_rearms(struct kunit *test)
{
	u64 drained;

	poll_test_reset(0);
	drained = tbv_path_poll_complete_ring(NULL, poll_test_poll,
					      poll_test_rearm);

	KUNIT_EXPECT_EQ(test, drained, (u64)0);
	KUNIT_EXPECT_EQ(test, PTC.cb_count, 0);
	KUNIT_EXPECT_EQ(test, PTC.rearm_count, 1);
}

static struct kunit_case tbv_path_poll_test_cases[] = {
	KUNIT_CASE(tbv_path_poll_drains_then_rearms),
	KUNIT_CASE(tbv_path_poll_empty_still_rearms),
	{}
};

static struct kunit_suite tbv_path_poll_test_suite = {
	.name = "thunderbolt_ibverbs_path_poll",
	.test_cases = tbv_path_poll_test_cases,
};
kunit_test_suite(tbv_path_poll_test_suite);
