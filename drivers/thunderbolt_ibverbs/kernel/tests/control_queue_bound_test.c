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

static struct kunit_case tbv_control_queue_bound_cases[] = {
	KUNIT_CASE(tbv_control_queue_stops_at_pool_capacity),
	{}
};

static struct kunit_suite tbv_control_queue_bound_suite = {
	.name = "thunderbolt_ibverbs_control_queue_bound",
	.test_cases = tbv_control_queue_bound_cases,
};

kunit_test_suite(tbv_control_queue_bound_suite);
