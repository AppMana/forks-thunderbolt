// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit: RC retransmit loss-recovery (deterministic simulation of the workload).
 *
 * Hardware (022<-021 ib_send_bw): under load ~0.3% of frames are lost; recovery
 * waits the retransmit timeout, and the measured single-loss recovery is ~69ms
 * (ACK-match age bimodal at fast or ~69ms). The first fix -- a FLAT 5ms cap --
 * was fatal: the total budget = interval * max_retries = 5*7 = 35ms < 69ms, so
 * a lost frame exhausted all retries before recovery and the QP died
 * (IBV_WC_GENERAL_ERR, observed on hardware). The backoff replaces it.
 *
 * This walks the retransmit schedule the timeout-work reap uses and pins the two
 * invariants the flat clamp violated:
 *   (a) FAST first retransmit -> first interval <= base (short tail), and
 *   (b) SURVIVES recovery     -> cumulative budget over max_retries >= the
 *       recovery deadline, so a lost frame can't exhaust the retry count first.
 *
 * Built into the module on CONFIG_KUNIT; run via tools/run-kunit.sh.
 */
#include <kunit/test.h>
#include <linux/jiffies.h>
#include "../tbv.h"

/* Cumulative retransmit budget over @max_retries with the backoff schedule. */
static unsigned long backoff_budget(unsigned long cap_j, unsigned long base_j,
				    u8 max_retries)
{
	unsigned long total = 0;
	u8 n;

	for (n = 0; n < max_retries; n++)
		total += tbv_send_retry_backoff_jiffies(cap_j, n, base_j,
							max_retries);
	return total;
}

static void tbv_backoff_recovers_without_exhausting(struct kunit *test)
{
	unsigned long cap = msecs_to_jiffies(67);	/* verbs ack_timeout */
	unsigned long base = msecs_to_jiffies(4);
	unsigned long recover_69 = msecs_to_jiffies(69);	/* observed single-loss recovery */
	unsigned long recover_200 = msecs_to_jiffies(200);	/* sustained-loss headroom */
	u8 max_retries = 7;

	/* (a) the first retransmit is fast -- no more than the base interval */
	KUNIT_EXPECT_LE(test,
		tbv_send_retry_backoff_jiffies(cap, 0, base, max_retries), base);
	/* no interval exceeds the verbs ack_timeout (the cap) */
	KUNIT_EXPECT_LE(test,
		tbv_send_retry_backoff_jiffies(cap, max_retries - 1, base,
					       max_retries), cap);
	/* (b) cumulative budget covers the observed recovery, with headroom */
	KUNIT_EXPECT_GE(test, backoff_budget(cap, base, max_retries), recover_69);
	KUNIT_EXPECT_GE(test, backoff_budget(cap, base, max_retries), recover_200);
}

/*
 * The flat clamp the backoff replaces: budget = clamp * max_retries. At 5ms x7 =
 * 35ms it cannot cover the 69ms recovery -- the exact QP death seen on hardware.
 * This documents why a flat cap is wrong (the backoff above must clear the same
 * bar the flat clamp fails).
 */
static void tbv_flat_clamp_would_exhaust(struct kunit *test)
{
	unsigned long flat_budget = msecs_to_jiffies(5) * 7;
	unsigned long recover_69 = msecs_to_jiffies(69);

	KUNIT_EXPECT_LT(test, flat_budget, recover_69);
}

static struct kunit_case tbv_backoff_test_cases[] = {
	KUNIT_CASE(tbv_backoff_recovers_without_exhausting),
	KUNIT_CASE(tbv_flat_clamp_would_exhaust),
	{}
};

static struct kunit_suite tbv_backoff_test_suite = {
	.name = "thunderbolt_ibverbs_retransmit_backoff",
	.test_cases = tbv_backoff_test_cases,
};
kunit_test_suite(tbv_backoff_test_suite);
