// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit: a stopped TX ring must not turn control traffic into an unbounded
 * GFP_ATOMIC allocation loop.
 *
 * Control packets have a path-owned pool sized from the TX ring. Once the ring
 * stops making progress that pool is the natural admission limit. The old
 * fallback allocated another packet for every ACK/credit/control frame, so a
 * live RX side could consume memory indefinitely while TX remained dead.
 */
#include <kunit/test.h>
#include "../tbv.h"

static void tbv_control_queue_stops_at_pool_capacity(struct kunit *test)
{
	u32 capacity = 0;
	u32 accepted = 0;
	u32 queued = 0;

	KUNIT_ASSERT_EQ(test,
			tbv_test_path_control_queue_bound(32, &capacity,
							  &accepted, &queued),
			0);
	KUNIT_ASSERT_GT(test, capacity, 0U);
	KUNIT_EXPECT_EQ_MSG(test, accepted, capacity,
			    "control enqueue accepted %u packets with a %u-packet pool while TX made no progress",
			    accepted, capacity);
	KUNIT_EXPECT_EQ_MSG(test, queued, capacity,
			    "control queue grew beyond its preallocated bound");
}

/*
 * Control frames share the same hardware descriptor ring as data. They need
 * priority over queued data, but they must not bypass the ring's total
 * in-flight ceiling: a burst of ACK/NAK traffic after one loss otherwise
 * expands a nominal 32-frame pipeline into hundreds of outstanding
 * descriptors and turns recovery into a second corruption storm.
 */
static void tbv_control_frames_obey_shared_inflight_ceiling(struct kunit *test)
{
	KUNIT_EXPECT_TRUE(test, tbv_path_tx_inflight_available(0, 32));
	KUNIT_EXPECT_TRUE(test, tbv_path_tx_inflight_available(31, 32));
	KUNIT_EXPECT_FALSE(test, tbv_path_tx_inflight_available(32, 32));
	KUNIT_EXPECT_FALSE(test, tbv_path_tx_inflight_available(185, 32));
	/* A defensive negative read must not permanently close the ring. */
	KUNIT_EXPECT_TRUE(test, tbv_path_tx_inflight_available(-1, 32));
}

static struct kunit_case tbv_control_queue_bound_cases[] = {
	KUNIT_CASE(tbv_control_queue_stops_at_pool_capacity),
	KUNIT_CASE(tbv_control_frames_obey_shared_inflight_ceiling),
	{}
};

static struct kunit_suite tbv_control_queue_bound_suite = {
	.name = "thunderbolt_ibverbs_control_queue_bound",
	.test_cases = tbv_control_queue_bound_cases,
};

kunit_test_suite(tbv_control_queue_bound_suite);
