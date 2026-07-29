// SPDX-License-Identifier: GPL-2.0
#include <kunit/test.h>
#include "../tbv.h"

static void tbv_rx_timeout_retries_within_deadline_budget(struct kunit *test)
{
	unsigned long timeout = msecs_to_jiffies(800);
	unsigned long first_delay = 0;
	u32 recoveries = 0;
	u32 wakeups = 0;
	bool active = true;

	KUNIT_ASSERT_EQ(test,
			tbv_test_rx_timeout_deadline(timeout, 2, &first_delay,
						     &wakeups, &recoveries,
						     &active),
			0);
	KUNIT_EXPECT_EQ(test, first_delay, timeout);
	KUNIT_EXPECT_EQ(test, wakeups, 3U);
	KUNIT_EXPECT_EQ(test, recoveries, 2U);
	KUNIT_EXPECT_FALSE(test, active);
}

static void tbv_rx_timeout_zero_retry_fails_once(struct kunit *test)
{
	unsigned long timeout = msecs_to_jiffies(800);
	unsigned long first_delay = 0;
	u32 recoveries = 1;
	u32 wakeups = 0;
	bool active = true;

	KUNIT_ASSERT_EQ(test,
			tbv_test_rx_timeout_deadline(timeout, 0, &first_delay,
						     &wakeups, &recoveries,
						     &active),
			0);
	KUNIT_EXPECT_EQ(test, first_delay, timeout);
	KUNIT_EXPECT_EQ(test, wakeups, 1U);
	KUNIT_EXPECT_EQ(test, recoveries, 0U);
	KUNIT_EXPECT_FALSE(test, active);
}

static struct kunit_case tbv_rx_timeout_deadline_cases[] = {
	KUNIT_CASE(tbv_rx_timeout_retries_within_deadline_budget),
	KUNIT_CASE(tbv_rx_timeout_zero_retry_fails_once),
	{}
};

static struct kunit_suite tbv_rx_timeout_deadline_suite = {
	.name = "thunderbolt_ibverbs_rx_timeout_deadline",
	.test_cases = tbv_rx_timeout_deadline_cases,
};

kunit_test_suite(tbv_rx_timeout_deadline_suite);
