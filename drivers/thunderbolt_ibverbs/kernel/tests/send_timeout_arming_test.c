// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit: send retransmit-timer ARMING (deterministic model of the ~100ms stall).
 *
 * Hardware (027<->019 NCCL ping-pong, tbv 0.2.24): ~0.5% of messages stalled
 * 85-112ms regardless of tbv_retransmit_base_ms (4ms or 1ms) -- every retried
 * ACK match aged >64ms (data_rx_ack_match_over_64ms == retransmits), 28/32
 * retransmits were spurious (duplicate ACKs). Root cause: the backoff deadline
 * (base << retries) was only a THRESHOLD checked when the QP timeout work ran,
 * and every arming site scheduled that work at
 * min3(ack_timeout, 1000ms, TBV_READ_RESP_RETRY_MS=100ms) = 100ms, so loss
 * recovery quantized to a 100ms grid.
 *
 * These tests pin the fixed contract:
 *   (a) the delay armed for a retryable send equals its backoff DEADLINE
 *       (bounded by the interval backstop), not the flat interval; and
 *   (b) re-arming is reduce-only: an armed-later timer must be pulled in for
 *       an earlier deadline, and never pushed out when already earlier.
 *
 * Built into the module on CONFIG_KUNIT; run via tools/run-kunit.sh.
 */
#include <kunit/test.h>
#include <linux/jiffies.h>
#include "../tbv.h"

static void tbv_send_arming_uses_deadline_not_interval(struct kunit *test)
{
	unsigned long timeout = msecs_to_jiffies(1070);	/* NCCL verbs timeout=18 */
	unsigned long interval = msecs_to_jiffies(100);	/* the min3 backstop */
	unsigned long base = msecs_to_jiffies(4);
	unsigned long now = jiffies;

	/* first retry: armed at base, NOT at the 100ms interval */
	KUNIT_EXPECT_EQ(test,
		tbv_qp_send_timeout_delay(timeout, interval, 0, base, 7, now, now),
		base);
	/* backoff grows with retries: retries=2 -> base<<2 */
	KUNIT_EXPECT_EQ(test,
		tbv_qp_send_timeout_delay(timeout, interval, 2, base, 7, now, now),
		base << 2);
	/* the interval remains a backstop: huge backoff clamps to interval */
	KUNIT_EXPECT_EQ(test,
		tbv_qp_send_timeout_delay(timeout, interval, 16, base, 32, now, now),
		interval);
	/* an already-overdue deadline fires immediately (1 jiffy) */
	KUNIT_EXPECT_EQ(test,
		tbv_qp_send_timeout_delay(timeout, interval, 0, base, 7,
					  now - msecs_to_jiffies(50), now),
		1UL);
	/* non-retryable sends keep the flat interval cadence */
	KUNIT_EXPECT_EQ(test,
		tbv_qp_send_timeout_delay(timeout, interval, 0, base, 0, now, now),
		interval);
	/* base=0 falls back to the verbs-derived flat schedule (never 0) */
	KUNIT_EXPECT_GT(test,
		tbv_qp_send_timeout_delay(timeout, interval, 0, 0, 7, now, now),
		0UL);
}

static void tbv_timeout_rearm_is_reduce_only(struct kunit *test)
{
	unsigned long now = jiffies;

	/* unarmed: always arm */
	KUNIT_EXPECT_TRUE(test,
		tbv_qp_timeout_rearm_needed(false, 0, now + 10, false));
	/* armed later, new deadline earlier: MUST pull in (the fixed bug) */
	KUNIT_EXPECT_TRUE(test,
		tbv_qp_timeout_rearm_needed(true, now + msecs_to_jiffies(100),
					    now + msecs_to_jiffies(4), false));
	/* armed earlier or equal: never push out */
	KUNIT_EXPECT_FALSE(test,
		tbv_qp_timeout_rearm_needed(true, now + msecs_to_jiffies(4),
					    now + msecs_to_jiffies(100), false));
	KUNIT_EXPECT_FALSE(test,
		tbv_qp_timeout_rearm_needed(true, now + 10, now + 10, false));
	/* replace overrides unconditionally (RNR path semantics) */
	KUNIT_EXPECT_TRUE(test,
		tbv_qp_timeout_rearm_needed(true, now + 10,
					    now + msecs_to_jiffies(640), true));
}

static struct kunit_case tbv_send_timeout_arming_cases[] = {
	KUNIT_CASE(tbv_send_arming_uses_deadline_not_interval),
	KUNIT_CASE(tbv_timeout_rearm_is_reduce_only),
	{}
};

static struct kunit_suite tbv_send_timeout_arming_suite = {
	.name = "thunderbolt_ibverbs_send_timeout_arming",
	.test_cases = tbv_send_timeout_arming_cases,
};
kunit_test_suite(tbv_send_timeout_arming_suite);
