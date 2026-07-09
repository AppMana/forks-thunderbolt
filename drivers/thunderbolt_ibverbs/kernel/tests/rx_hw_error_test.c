// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit: NHI-reported RX frame errors must never be parsed.
 *
 * Frame-mode RX sets RING_DESC_CRC_ERROR on a failed CRC and
 * RING_DESC_BUFFER_OVERRUN when the frame did not fit. The driver used to read
 * frame->buf regardless: a corrupt frame was parsed as a native header (landing
 * in data_rx_bad_header with a garbage opcode) or, mid raw stream, scattered
 * into a user MR as payload. The hardware had already told us the bytes were
 * bad.
 *
 * This pins the gate. It also documents the diagnostic value: separating
 * data_rx_crc_error / data_rx_overrun from data_rx_bad_header answers whether
 * the residual bad-header rate on a healthy link is wire corruption or a
 * software framing bug.
 */
#include <kunit/test.h>
#include <linux/thunderbolt.h>
#include "../tbv.h"

/*
 * enum ring_desc_flags ALIASES tx-side and rx-side meanings on the same bits:
 *   0x1 = RING_DESC_ISOCH (tx)  == RING_DESC_CRC_ERROR (rx)
 *   0x4 = RING_DESC_POSTED (tx) == RING_DESC_BUFFER_OVERRUN (rx)
 * On an RX completion the NHI writes descriptor status, so bit 2 means overrun.
 * Reading it as POSTED (and treating it as benign) would defeat the gate;
 * treating a TX frame's POSTED as an overrun would drop every frame. The gate
 * is therefore only valid on the RX completion path.
 */
static void tbv_hw_error_flags_are_rejected(struct kunit *test)
{
	KUNIT_EXPECT_TRUE(test, tbv_frame_hw_error(RING_DESC_CRC_ERROR));
	KUNIT_EXPECT_TRUE(test, tbv_frame_hw_error(RING_DESC_BUFFER_OVERRUN));
	KUNIT_EXPECT_TRUE(test,
		tbv_frame_hw_error(RING_DESC_CRC_ERROR |
				   RING_DESC_BUFFER_OVERRUN));
	/* a clean, completed RX frame carries neither error bit */
	KUNIT_EXPECT_FALSE(test, tbv_frame_hw_error(RING_DESC_COMPLETED));
	KUNIT_EXPECT_FALSE(test, tbv_frame_hw_error(0));
	/* an error must be detected alongside the completion status bit */
	KUNIT_EXPECT_TRUE(test,
		tbv_frame_hw_error(RING_DESC_COMPLETED | RING_DESC_CRC_ERROR));
	KUNIT_EXPECT_TRUE(test,
		tbv_frame_hw_error(RING_DESC_COMPLETED |
				   RING_DESC_BUFFER_OVERRUN));
}

static struct kunit_case tbv_rx_hw_error_cases[] = {
	KUNIT_CASE(tbv_hw_error_flags_are_rejected),
	{}
};

static struct kunit_suite tbv_rx_hw_error_suite = {
	.name = "thunderbolt_ibverbs_rx_hw_error",
	.test_cases = tbv_rx_hw_error_cases,
};
kunit_test_suite(tbv_rx_hw_error_suite);
