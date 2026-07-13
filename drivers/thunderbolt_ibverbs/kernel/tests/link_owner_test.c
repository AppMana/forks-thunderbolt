// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit: link data-path owner arbitration (usb4_rdma <-> thunderbolt_net).
 *
 * Gates the deterministic, reload-free RUNTIME handoff. Note the zombie's
 * actual root cause is tbnet's one-shot session never revalidating its DMA
 * tunnel (see link_owner.h for the corrected post-mortem and
 * thunderbolt_net's session-verify fix); this suite covers the exclusivity
 * POLICY layer -- one data-path owner per link at a time -- and the planner
 * that keeps applying it wedge-free.
 *
 * This suite drives the pure arbitration logic from the REAL header (no mirror)
 * and asserts:
 *   1. the exclusivity invariant (tbnet owns the link iff no tbv tunnel),
 *   2. the four owner transitions produce the right release/claim action,
 *   3. idempotency: re-applying an owner never churns tunnels (wedge-free),
 *   4. an end-to-end model where applying the plan flips link ownership --
 *      the ping-works-after-handoff / ib_send_lat-works-after-handback
 *      contract, in software.
 *
 * Red-first: build with TBV_LINK_OWNER_ENFORCE_EXCLUSIVITY=0 to model the
 * pre-fix driver (tbv never releases its tunnels on handoff) and the
 * end-to-end assertion fails. The shipped driver enforces exclusivity (=1).
 * Run via tools/run-kunit.sh.
 */
#include <kunit/test.h>
#include <linux/types.h>

#include "../link_owner.h"

/*
 * Lockstep lever with the driver's handoff wiring (debugfs toggle iterating
 * rails -> tbv_path_disable_tunnel/enable_tunnel). 1 = the handoff actually
 * releases/claims tbv's DMA tunnels per the plan. 0 = model the pre-fix driver
 * that leaves tbv tunnels live regardless of owner (the zombie reproduction).
 */
#ifndef TBV_LINK_OWNER_ENFORCE_EXCLUSIVITY
#define TBV_LINK_OWNER_ENFORCE_EXCLUSIVITY 1
#endif

/* Minimal model of one link's DMA-tunnel state driven by the real planner. */
struct lo_link {
	enum tbv_link_owner owner;
	bool tbv_tunnels_live;
};

/* Apply the real plan for @desired to @link, mutating tunnel state like the
 * driver's rail iteration would. */
static void lo_apply(struct lo_link *link, enum tbv_link_owner desired)
{
	enum tbv_link_owner_action action =
		tbv_link_owner_plan(desired, link->tbv_tunnels_live);

	link->owner = desired;

	if (!TBV_LINK_OWNER_ENFORCE_EXCLUSIVITY)
		return; /* pre-fix: owner recorded but tunnels never toggled */

	switch (action) {
	case TBV_LINK_ACTION_RELEASE_TBV:
		link->tbv_tunnels_live = false;
		break;
	case TBV_LINK_ACTION_CLAIM_TBV:
		link->tbv_tunnels_live = true;
		break;
	case TBV_LINK_ACTION_NONE:
		break;
	}
}

static void tbv_link_owner_invariant(struct kunit *test)
{
	/* tbnet passes iff no tbv DMA tunnel is co-resident. */
	KUNIT_EXPECT_TRUE(test, tbv_link_tbnet_can_pass(false));
	KUNIT_EXPECT_FALSE(test, tbv_link_tbnet_can_pass(true));

	KUNIT_EXPECT_TRUE(test, tbv_link_owner_rdma_data_allowed(TBV_LINK_OWNER_RDMA));
	KUNIT_EXPECT_FALSE(test, tbv_link_owner_rdma_data_allowed(TBV_LINK_OWNER_TBNET));
}

static void tbv_link_owner_transitions(struct kunit *test)
{
	/* RDMA owner, tbv tunnels already live -> no-op. */
	KUNIT_EXPECT_EQ(test, TBV_LINK_ACTION_NONE,
			tbv_link_owner_plan(TBV_LINK_OWNER_RDMA, true));
	/* Hand to tbnet while tbv tunnels live -> release them. */
	KUNIT_EXPECT_EQ(test, TBV_LINK_ACTION_RELEASE_TBV,
			tbv_link_owner_plan(TBV_LINK_OWNER_TBNET, true));
	/* tbnet owner, tbv tunnels already released -> no-op. */
	KUNIT_EXPECT_EQ(test, TBV_LINK_ACTION_NONE,
			tbv_link_owner_plan(TBV_LINK_OWNER_TBNET, false));
	/* Hand back to RDMA while tbv tunnels released -> claim them. */
	KUNIT_EXPECT_EQ(test, TBV_LINK_ACTION_CLAIM_TBV,
			tbv_link_owner_plan(TBV_LINK_OWNER_RDMA, false));
}

static void tbv_link_owner_idempotent(struct kunit *test)
{
	struct lo_link link = { .owner = TBV_LINK_OWNER_RDMA,
				.tbv_tunnels_live = true };
	int i;

	/* Repeated identical writes must never churn tunnels (wedge-free). */
	for (i = 0; i < 5; i++) {
		lo_apply(&link, TBV_LINK_OWNER_RDMA);
		KUNIT_EXPECT_TRUE(test, link.tbv_tunnels_live);
	}
	lo_apply(&link, TBV_LINK_OWNER_TBNET);
	for (i = 0; i < 5; i++) {
		lo_apply(&link, TBV_LINK_OWNER_TBNET);
		KUNIT_EXPECT_FALSE(test, link.tbv_tunnels_live);
	}
}

static void tbv_link_owner_end_to_end(struct kunit *test)
{
	struct lo_link link = { .owner = TBV_LINK_OWNER_RDMA,
				.tbv_tunnels_live = true };

	/* Steady state: RDMA owns -> tbnet does not own the data path. */
	KUNIT_EXPECT_FALSE(test, tbv_link_tbnet_can_pass(link.tbv_tunnels_live));

	/* Operator hands the link to tbnet. */
	lo_apply(&link, TBV_LINK_OWNER_TBNET);
	/* After handoff tbnet can ping. Pre-fix (=0) leaves tunnels live -> FAIL. */
	KUNIT_EXPECT_TRUE(test, tbv_link_tbnet_can_pass(link.tbv_tunnels_live));

	/* Hand back to RDMA; tbv reclaims its tunnels. */
	lo_apply(&link, TBV_LINK_OWNER_RDMA);
	KUNIT_EXPECT_TRUE(test, link.tbv_tunnels_live);
	KUNIT_EXPECT_FALSE(test, tbv_link_tbnet_can_pass(link.tbv_tunnels_live));
}

static struct kunit_case tbv_link_owner_cases[] = {
	KUNIT_CASE(tbv_link_owner_invariant),
	KUNIT_CASE(tbv_link_owner_transitions),
	KUNIT_CASE(tbv_link_owner_idempotent),
	KUNIT_CASE(tbv_link_owner_end_to_end),
	{}
};

static struct kunit_suite tbv_link_owner_suite = {
	.name = "tbv_link_owner",
	.test_cases = tbv_link_owner_cases,
};
kunit_test_suite(tbv_link_owner_suite);

MODULE_LICENSE("GPL");
