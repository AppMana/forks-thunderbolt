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
#include "../../proto/native_data.h"

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

/*
 * A dropped PATH_CREDIT frame is a permanent shortfall under the delta-only
 * scheme: nothing ever restates it. The absolute resync recomputes exactly the
 * missing count from the peer's cumulative total.
 */
static void tbv_credit_lost_frame_heals_on_resync(struct kunit *test)
{
	u32 credits = 0;
	u32 max = 0;

	/* The whole window returned in one frame, and that frame is lost. */
	KUNIT_ASSERT_EQ(test,
			tbv_test_path_credit_resync(64, 64, false, &credits,
						    &max),
			0);
	KUNIT_EXPECT_EQ_MSG(test, credits, 0U,
			    "delta-only credit return should not recover a lost frame");

	KUNIT_ASSERT_EQ(test,
			tbv_test_path_credit_resync(64, 64, true, &credits,
						    &max),
			0);
	KUNIT_EXPECT_EQ_MSG(test, credits, 64U,
			    "absolute resync did not restore the lost credits");
}

/* A partial loss must recover exactly the missing credits, not re-grant all. */
static void tbv_credit_partial_loss_heals_exactly(struct kunit *test)
{
	u32 credits = 0;
	u32 max = 0;

	KUNIT_ASSERT_EQ(test,
			tbv_test_path_credit_resync(64, 0, true, &credits,
						    &max),
			0);
	KUNIT_EXPECT_EQ(test, credits, 64U);

	KUNIT_ASSERT_EQ(test,
			tbv_test_path_credit_resync(64, 17, true, &credits,
						    &max),
			0);
	KUNIT_EXPECT_EQ(test, credits, 64U);

	KUNIT_ASSERT_EQ(test,
			tbv_test_path_credit_resync(64, 17, false, &credits,
						    &max),
			0);
	KUNIT_EXPECT_EQ(test, credits, 47U);
}

/* The resync arithmetic is modular so the cumulative counters may wrap. */
static void tbv_credit_resync_delta_wraps(struct kunit *test)
{
	u32 delta = 0;

	KUNIT_EXPECT_EQ(test, tbv_native_data_resync_delta(0, 7), 7U);
	KUNIT_EXPECT_EQ(test, tbv_native_data_resync_delta(U32_MAX - 2, 5), 8U);
	KUNIT_EXPECT_EQ(test, tbv_native_data_resync_delta(9, 9), 0U);
	KUNIT_EXPECT_TRUE(test,
			 tbv_native_data_resync_forward(U32_MAX - 2, 5,
							&delta));
	KUNIT_EXPECT_EQ(test, delta, 8U);
	KUNIT_EXPECT_FALSE(test,
			  tbv_native_data_resync_forward(100, 99, &delta));
}

static void tbv_credit_stale_sync_does_not_refill(struct kunit *test)
{
	u64 recovered = U64_MAX;
	u32 credits = U32_MAX;
	u32 seen = 0;

	KUNIT_ASSERT_EQ(test,
			tbv_test_path_credit_stale_sync(&credits, &seen,
							&recovered),
			0);
	KUNIT_EXPECT_EQ(test, credits, 0U);
	KUNIT_EXPECT_EQ(test, seen, 100U);
	KUNIT_EXPECT_EQ(test, recovered, 0ULL);
}

/* The resync frame must survive a build/parse round trip and validate. */
static void tbv_credit_resync_frame_round_trips(struct kunit *test)
{
	struct tbv_native_data_header hdr = {};
	struct tbv_native_data_header out = {};
	u8 frame[TBV_NATIVE_DATA_HDR_SIZE];

	hdr.opcode = TBV_NATIVE_DATA_OP_PATH_CREDIT_SYNC;
	hdr.frag_offset = 4242;

	KUNIT_ASSERT_EQ(test,
			tbv_native_data_build_header(frame, sizeof(frame), &hdr),
			(int)TBV_NATIVE_DATA_HDR_SIZE);
	KUNIT_ASSERT_EQ(test,
			tbv_native_data_parse_header(frame, sizeof(frame), &out),
			0);
	KUNIT_EXPECT_TRUE(test, tbv_native_data_valid_path_credit_sync(&out));
	KUNIT_EXPECT_EQ(test, out.frag_offset, 4242U);

	/* A delta credit frame must never validate as a resync. */
	out.opcode = TBV_NATIVE_DATA_OP_PATH_CREDIT;
	out.imm_data = 8;
	KUNIT_EXPECT_FALSE(test, tbv_native_data_valid_path_credit_sync(&out));
}

static struct kunit_case tbv_credit_stall_recovery_cases[] = {
	KUNIT_CASE(tbv_credit_lost_frame_heals_on_resync),
	KUNIT_CASE(tbv_credit_partial_loss_heals_exactly),
	KUNIT_CASE(tbv_credit_resync_delta_wraps),
	KUNIT_CASE(tbv_credit_stale_sync_does_not_refill),
	KUNIT_CASE(tbv_credit_resync_frame_round_trips),
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
