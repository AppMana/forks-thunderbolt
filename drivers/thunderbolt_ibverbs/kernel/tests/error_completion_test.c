// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit: a WR that completes IN ERROR must always generate a CQE.
 *
 * tbv_send_complete() and tbv_read_complete() pushed a WC only when the WR
 * carried IB_SEND_SIGNALED. IBTA requires a WQE completing in error to
 * generate a completion regardless of signaling, and requires the remaining
 * WQEs to be flushed with IB_WC_WR_FLUSH_ERR once the QP is in ERR. Without
 * that, an unsignaled WR that times out, exhausts its RNR retries or is
 * flushed simply disappears: the application sees the send queue drain and
 * never learns the transfer failed.
 *
 * The status mapping is part of the same contract. A remote error ACK stores
 * -EIO, which was reported as IB_WC_WR_FLUSH_ERR -- indistinguishable from a
 * local flush, and misleading, because the peer rejected the operation.
 *
 * Out of scope and NOT covered here: ib_dispatch_event() for the async
 * IB_EVENT_QP_FATAL / IB_EVENT_QP_REQ_ERR notifications, which the driver
 * still does not emit at all.
 *
 * Built into the module on CONFIG_KUNIT; run via tools/run-kunit.sh.
 */
#include <kunit/test.h>
#include <rdma/ib_verbs.h>
#include "../tbv.h"

static void tbv_unsignaled_error_send_completes(struct kunit *test)
{
	int wc_status = -1;
	u32 pushed = 0;

	/* retry budget exhausted on an unsignaled WR */
	KUNIT_ASSERT_EQ(test,
			tbv_test_send_complete_wc(false, -ETIMEDOUT, &pushed,
						  &wc_status),
			0);
	KUNIT_EXPECT_EQ(test, pushed, 1U);
	KUNIT_EXPECT_EQ(test, wc_status, (int)IB_WC_RETRY_EXC_ERR);

	/* RNR retries exhausted */
	KUNIT_ASSERT_EQ(test,
			tbv_test_send_complete_wc(false, -EAGAIN, &pushed,
						  &wc_status),
			0);
	KUNIT_EXPECT_EQ(test, pushed, 1U);
	KUNIT_EXPECT_EQ(test, wc_status, (int)IB_WC_RNR_RETRY_EXC_ERR);

	/* flushed because the QP went to ERR */
	KUNIT_ASSERT_EQ(test,
			tbv_test_send_complete_wc(false, -ECANCELED, &pushed,
						  &wc_status),
			0);
	KUNIT_EXPECT_EQ(test, pushed, 1U);
	KUNIT_EXPECT_EQ(test, wc_status, (int)IB_WC_WR_FLUSH_ERR);
}

static void tbv_unsignaled_success_send_is_silent(struct kunit *test)
{
	int wc_status = -1;
	u32 pushed = 0;

	/* the signaling rule still governs SUCCESSFUL completions */
	KUNIT_ASSERT_EQ(test,
			tbv_test_send_complete_wc(false, 0, &pushed,
						  &wc_status),
			0);
	KUNIT_EXPECT_EQ(test, pushed, 0U);

	KUNIT_ASSERT_EQ(test,
			tbv_test_send_complete_wc(true, 0, &pushed,
						  &wc_status),
			0);
	KUNIT_EXPECT_EQ(test, pushed, 1U);
	KUNIT_EXPECT_EQ(test, wc_status, (int)IB_WC_SUCCESS);
}

static void tbv_remote_error_ack_maps_to_remote_status(struct kunit *test)
{
	int wc_status = -1;
	u32 pushed = 0;

	/* TBV_NATIVE_SEND_ACK_ERROR from the peer stores -EIO */
	KUNIT_ASSERT_EQ(test,
			tbv_test_send_complete_wc(true, -EIO, &pushed,
						  &wc_status),
			0);
	KUNIT_EXPECT_EQ(test, pushed, 1U);
	KUNIT_EXPECT_EQ(test, wc_status, (int)IB_WC_REM_OP_ERR);

	/* a peer rejecting the rkey/range is an access error */
	KUNIT_ASSERT_EQ(test,
			tbv_test_send_complete_wc(true, -EACCES, &pushed,
						  &wc_status),
			0);
	KUNIT_EXPECT_EQ(test, pushed, 1U);
	KUNIT_EXPECT_EQ(test, wc_status, (int)IB_WC_REM_ACCESS_ERR);
}

static void tbv_unsignaled_error_read_completes(struct kunit *test)
{
	int wc_status = -1;
	u32 pushed = 0;

	KUNIT_ASSERT_EQ(test,
			tbv_test_read_complete_wc(false, -ETIMEDOUT, &pushed,
						  &wc_status),
			0);
	KUNIT_EXPECT_EQ(test, pushed, 1U);
	KUNIT_EXPECT_EQ(test, wc_status, (int)IB_WC_RETRY_EXC_ERR);

	KUNIT_ASSERT_EQ(test,
			tbv_test_read_complete_wc(false, -ECANCELED, &pushed,
						  &wc_status),
			0);
	KUNIT_EXPECT_EQ(test, pushed, 1U);
	KUNIT_EXPECT_EQ(test, wc_status, (int)IB_WC_WR_FLUSH_ERR);

	/* remote error on a READ response */
	KUNIT_ASSERT_EQ(test,
			tbv_test_read_complete_wc(false, -EIO, &pushed,
						  &wc_status),
			0);
	KUNIT_EXPECT_EQ(test, pushed, 1U);
	KUNIT_EXPECT_EQ(test, wc_status, (int)IB_WC_REM_OP_ERR);

	/* and an unsignaled successful READ still stays silent */
	KUNIT_ASSERT_EQ(test,
			tbv_test_read_complete_wc(false, 0, &pushed,
						  &wc_status),
			0);
	KUNIT_EXPECT_EQ(test, pushed, 0U);
}

static struct kunit_case tbv_error_completion_cases[] = {
	KUNIT_CASE(tbv_unsignaled_error_send_completes),
	KUNIT_CASE(tbv_unsignaled_success_send_is_silent),
	KUNIT_CASE(tbv_remote_error_ack_maps_to_remote_status),
	KUNIT_CASE(tbv_unsignaled_error_read_completes),
	{}
};

static struct kunit_suite tbv_error_completion_suite = {
	.name = "thunderbolt_ibverbs_error_completion",
	.test_cases = tbv_error_completion_cases,
};

kunit_test_suite(tbv_error_completion_suite);
