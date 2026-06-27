// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit: reorder-gap NAK decision.
 *
 * Measured on hardware (022<-021, ib_send_bw): under load ~0.3% of frames go
 * missing/severely-reordered; the receiver buffers later frames in its reorder
 * window (reorder_buffered=836) but has NO NAK path, so the gap waits out the
 * SENDER's ~67ms ack timeout before the missing frame is retransmitted -- the
 * latency tail (bimodal ACK-match age: fast or ~69ms, nothing between).
 *
 * The fix adds a NAK: once the reorder window has been blocked on a missing PSN
 * long enough that it is loss (not still-in-flight reorder), the receiver NAKs
 * rx_expected_psn so the sender fast-retransmits in ~1ms. This pins the decision:
 *   - empty window            -> no NAK
 *   - fresh gap               -> no NAK (could be genuine in-flight reorder)
 *   - gap older than min_gap  -> NAK the expected (missing) PSN
 *
 * Built into the module on CONFIG_KUNIT; run via tools/run-kunit.sh.
 */
#include <kunit/test.h>
#include <linux/jiffies.h>
#include "../tbv.h"

static void tbv_reorder_nak_decision_cases(struct kunit *test)
{
	u32 nak = 0;

	/* Empty reorder window: nothing missing -> no NAK. */
	KUNIT_EXPECT_FALSE(test,
		tbv_rx_reorder_nak_decision(true, 0, 100, 1000, 10, &nak));

	/* Fresh gap (oldest buffered just now): may still be in-flight -> no NAK. */
	KUNIT_EXPECT_FALSE(test,
		tbv_rx_reorder_nak_decision(false, 1000, 100, 1005, 10, &nak));

	/* Gap persisted past min_gap -> NAK the expected (missing) PSN. */
	nak = 0;
	KUNIT_EXPECT_TRUE(test,
		tbv_rx_reorder_nak_decision(false, 1000, 100, 1020, 10, &nak));
	KUNIT_EXPECT_EQ(test, nak, 100u);

	/* A different expected PSN is reported back unchanged. */
	nak = 0;
	KUNIT_EXPECT_TRUE(test,
		tbv_rx_reorder_nak_decision(false, 5000, 4680615, 5050, 10, &nak));
	KUNIT_EXPECT_EQ(test, nak, 4680615u);
}

static struct kunit_case tbv_reorder_nak_test_cases[] = {
	KUNIT_CASE(tbv_reorder_nak_decision_cases),
	{}
};

static struct kunit_suite tbv_reorder_nak_test_suite = {
	.name = "thunderbolt_ibverbs_reorder_nak",
	.test_cases = tbv_reorder_nak_test_cases,
};
kunit_test_suite(tbv_reorder_nak_test_suite);
