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

static void tbv_rx_progress_restarts_idle_deadline(struct kunit *test)
{
	unsigned long timeout = msecs_to_jiffies(800);
	unsigned long progress_after = msecs_to_jiffies(300);
	unsigned long remaining = 0;
	u8 retries_after_progress = 0xff;
	bool active_at_old_deadline = false;

	KUNIT_ASSERT_EQ(test,
			tbv_test_rx_progress_extends_deadline(
				timeout, progress_after,
				&active_at_old_deadline,
				&retries_after_progress, &remaining),
			0);
	KUNIT_EXPECT_TRUE(test, active_at_old_deadline);
	KUNIT_EXPECT_EQ(test, retries_after_progress, (u8)0);
	KUNIT_EXPECT_EQ(test, remaining, progress_after);
}

static struct kunit_case tbv_rx_timeout_deadline_cases[] = {
	KUNIT_CASE(tbv_rx_timeout_retries_within_deadline_budget),
	KUNIT_CASE(tbv_rx_timeout_zero_retry_fails_once),
	KUNIT_CASE(tbv_rx_progress_restarts_idle_deadline),
	{}
};

static struct kunit_suite tbv_rx_timeout_deadline_suite = {
	.name = "thunderbolt_ibverbs_rx_timeout_deadline",
	.test_cases = tbv_rx_timeout_deadline_cases,
};

kunit_test_suite(tbv_rx_timeout_deadline_suite);
