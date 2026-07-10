// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit: Task 2 (local-completion WC) contract -- design-only, unwired.
 *
 * Today a native RC send WC fires only from the SEND_ACK handler, a full RTT
 * after the local NHI already transmitted. Task 2 fires it at local TX-ring
 * completion, with reliability (NAK/retransmit) async behind it. The zcopy CRC
 * investigation discovered the constraint this must respect: the earliest safe
 * completion point differs by send class, because it is set by where the
 * payload lives during TX.
 *
 *   copied (framed): payload is memcpy'd into a kernel ring frame at post, so
 *     the user buffer is free immediately -- WC may fire AT_POST.
 *   zero-copy (split): the NHI DMA-reads the live pinned user MR pages, so the
 *     buffer cannot be reused until local TX-ring completion -- WC must NOT
 *     fire before that, or NCCL reuses the buffer under the NHI's read and
 *     tears the frame (the reuse race the current ACK-gating prevents by
 *     being strictly later than both floors).
 *
 * These pin the asymmetry so the implementation cannot regress the floor.
 * Built into the module on CONFIG_KUNIT; run via tools/run-kunit.sh.
 */
#include <kunit/test.h>
#include "../tbv.h"

static void tbv_wc_earliest_point_by_class(struct kunit *test)
{
	/* copied: the user buffer is staged at post, so post is the floor */
	KUNIT_EXPECT_EQ(test, tbv_send_wc_earliest_point(false),
			TBV_WC_AT_POST);
	/* zero-copy: the NHI reads the live buffer, so local TX is the floor */
	KUNIT_EXPECT_EQ(test, tbv_send_wc_earliest_point(true),
			TBV_WC_AT_LOCAL_TX);
}

static void tbv_wc_copied_fires_without_waiting_for_tx(struct kunit *test)
{
	enum tbv_wc_completion_point p = tbv_send_wc_earliest_point(false);

	/* posted but the local ring has NOT completed: a copied WC may fire */
	KUNIT_EXPECT_TRUE(test,
		tbv_send_wc_may_fire(p, /*posted=*/true,
				     /*local_tx_complete=*/false,
				     /*remote_acked=*/false));
	/* not yet posted: nothing may fire */
	KUNIT_EXPECT_FALSE(test,
		tbv_send_wc_may_fire(p, false, false, false));
}

static void tbv_wc_zcopy_must_wait_for_local_tx(struct kunit *test)
{
	enum tbv_wc_completion_point p = tbv_send_wc_earliest_point(true);

	/*
	 * The load-bearing assertion: a zcopy send that is posted but whose
	 * local TX ring has NOT completed MUST NOT complete -- firing here is
	 * exactly the reuse race. A copied send (above) may; a zcopy send may
	 * not. That asymmetry is the whole design.
	 */
	KUNIT_EXPECT_FALSE(test,
		tbv_send_wc_may_fire(p, /*posted=*/true,
				     /*local_tx_complete=*/false,
				     /*remote_acked=*/false));
	/* once the local ring completes, the read is done -> buffer free */
	KUNIT_EXPECT_TRUE(test,
		tbv_send_wc_may_fire(p, true, /*local_tx_complete=*/true,
				     false));
	/* a remote ACK alone, without local TX completion, is NOT sufficient
	 * to reason about the buffer (though in practice ACK implies TX done);
	 * the floor is explicitly local TX completion.
	 */
	KUNIT_EXPECT_FALSE(test,
		tbv_send_wc_may_fire(p, true, false, /*remote_acked=*/true));
}

static void tbv_wc_ack_baseline_is_conservative(struct kunit *test)
{
	/*
	 * The current behavior (AT_REMOTE_ACK) is strictly later than both
	 * per-class floors, which is why it is safe by construction for both.
	 * Task 2 relaxes it to the floor, never below.
	 */
	KUNIT_EXPECT_FALSE(test,
		tbv_send_wc_may_fire(TBV_WC_AT_REMOTE_ACK, true, true, false));
	KUNIT_EXPECT_TRUE(test,
		tbv_send_wc_may_fire(TBV_WC_AT_REMOTE_ACK, true, true, true));
}

static struct kunit_case tbv_send_wc_completion_cases[] = {
	KUNIT_CASE(tbv_wc_earliest_point_by_class),
	KUNIT_CASE(tbv_wc_copied_fires_without_waiting_for_tx),
	KUNIT_CASE(tbv_wc_zcopy_must_wait_for_local_tx),
	KUNIT_CASE(tbv_wc_ack_baseline_is_conservative),
	{}
};

static struct kunit_suite tbv_send_wc_completion_suite = {
	.name = "thunderbolt_ibverbs_send_wc_completion",
	.test_cases = tbv_send_wc_completion_cases,
};
kunit_test_suite(tbv_send_wc_completion_suite);
