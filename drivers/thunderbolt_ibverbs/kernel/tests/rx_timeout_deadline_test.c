// SPDX-License-Identifier: GPL-2.0
#include <kunit/test.h>
#include "../tbv.h"

static void tbv_rx_timeout_has_one_deadline_wakeup(struct kunit *test)
{
	unsigned long timeout = msecs_to_jiffies(800);
	unsigned long first_delay = 0;
	u32 wakeups = 0;
	bool active = true;

	KUNIT_ASSERT_EQ(test,
			tbv_test_rx_timeout_deadline(timeout, &first_delay,
						     &wakeups, &active),
			0);
	KUNIT_EXPECT_EQ(test, first_delay, timeout);
	KUNIT_EXPECT_EQ(test, wakeups, 1U);
	KUNIT_EXPECT_FALSE(test, active);
}

static struct kunit_case tbv_rx_timeout_deadline_cases[] = {
	KUNIT_CASE(tbv_rx_timeout_has_one_deadline_wakeup),
	{}
};

static struct kunit_suite tbv_rx_timeout_deadline_suite = {
	.name = "thunderbolt_ibverbs_rx_timeout_deadline",
	.test_cases = tbv_rx_timeout_deadline_cases,
};

kunit_test_suite(tbv_rx_timeout_deadline_suite);
