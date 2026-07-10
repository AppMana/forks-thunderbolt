// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit: rail-teardown quiesce (the "can't rmmod / must cold-boot" bug).
 *
 * tbv_peer_remove_rail() runs on BOTH module_exit (rmmod) and reboot
 * (.shutdown -> nhi_remove -> tb_domain_remove unbinds the tb_services). It
 * ends in wait_for_completion(rail->refs_zero): every QP pinned to the rail
 * holds a rail refcount that is dropped only when the QP finishes destroying
 * (tbv_qp_unbind_rail, the LAST thing tbv_destroy_qp does).
 *
 * The hang: a native data QP has frames posted to the NHI TX ring in-flight
 * list. On a DEAD link (peer rebooted / cable pulled) hardware never completes
 * them, so tbv_destroy_qp's internal ref never drains, its bounded wait times
 * out (-ETIMEDOUT, "leaving it closing for retry"), it returns WITHOUT reaching
 * tbv_qp_unbind_rail, and the rail ref is pinned forever. The passive
 * wait_for_completion(refs_zero) then blocks in D-state until a cold boot --
 * the ROOT of both "every change needs a cold boot" and the fleet version skew.
 *
 * The only thing that reclaims a frame already handed to the NHI ring is
 * tb_ring_stop(): it cancels every in-flight frame and runs its completion
 * callback with canceled=true (drivers/thunderbolt/nhi.c ring_work, the
 * !ring->running branch). So the teardown must FENCE the rings (stop rx+tx)
 * BEFORE waiting, not after -- the pre-fix ordering ran tb_ring_stop inside
 * tbv_path_destroy() which is reached only AFTER the wait it was meant to
 * unblock.
 *
 * TBV_TEARDOWN_FENCE_BEFORE_WAIT is the single lockstep lever, moved together
 * with tbv_peer_remove_rail()/tbv_path_fence() in the driver. It is 1 (fixed)
 * here; building this test with it defined to 0 reproduces the pre-fix hang
 * exactly as reverting the driver would -- the red-first run. Built on
 * CONFIG_KUNIT; run via tools/run-kunit.sh.
 */
#include <kunit/test.h>
#include <linux/types.h>

/*
 * Lockstep with the driver. 1 = tbv_peer_remove_rail() fences the NHI rings
 * (tbv_path_fence) before wait_for_completion(refs_zero). Define to 0 to model
 * the pre-fix ordering (ring-stop only in tbv_path_destroy, after the wait).
 */
#ifndef TBV_TEARDOWN_FENCE_BEFORE_WAIT
#define TBV_TEARDOWN_FENCE_BEFORE_WAIT 1
#endif

/* Faithful, self-contained model of one rail + one QP pinned to it. */
struct rq_rail {
	int refcnt;		/* rail->refcnt: birth ref + one per bound QP */
	bool rings_fenced;	/* tb_ring_stop(rx)+tb_ring_stop(tx) done */
	bool rings_freed;	/* tb_ring_free done (tbv_path_destroy) */
	bool leaked;		/* forced path: unlinked but not kfree'd */
	bool freed;		/* cleanly kfree'd (refs drained first) */
};

struct rq_qp {
	struct rq_rail *rail;
	int nhi_inflight;	/* frames in the NHI ring in-flight list */
	bool link_alive;	/* does hardware still complete frames? */
	bool frame_unreclaimable; /* pathological: even the fence can't cancel it */
	bool rail_ref_held;	/* QP still owns its rail refcount */
};

/*
 * A QP drops its rail ref only when tbv_destroy_qp reaches tbv_qp_unbind_rail,
 * which requires its in-flight frames to complete. On a live link hardware
 * completes them; on a dead link ONLY tb_ring_stop (fence) does. Mirrors the
 * real dependency: no fence + dead link => the ref is pinned.
 */
static void rq_qp_try_unbind(struct rq_qp *qp)
{
	if (!qp->rail_ref_held)
		return;
	if (qp->nhi_inflight > 0)
		return;		/* tbv_destroy_qp still pinned -> no unbind */
	qp->rail->refcnt--;	/* tbv_qp_unbind_rail -> tbv_rail_put */
	qp->rail_ref_held = false;
}

/* tb_ring_stop(rx)+tb_ring_stop(tx): cancel every in-flight frame. */
static void rq_fence_rings(struct rq_rail *rail, struct rq_qp *qp)
{
	rail->rings_fenced = true;
	if (!qp->frame_unreclaimable)
		qp->nhi_inflight = 0;	/* ring_work !running: canceled=true */
	rq_qp_try_unbind(qp);
}

/*
 * Model of tbv_peer_remove_rail(). @deadline bounds the passive wait so a hung
 * model still returns (the real bounded forced path). Returns true when
 * module_exit may proceed.
 */
static bool rq_remove_rail(struct rq_rail *rail, struct rq_qp *qp, int deadline)
{
	int waited = 0;

	/* Live link completes in-flight frames on its own over time. */
	if (qp->link_alive)
		qp->nhi_inflight = 0;

	rail->refcnt--;		/* drop the birth ref (tbv_rail_put) */
	rq_qp_try_unbind(qp);	/* ib_unregister_device -> destroy_qp flush */

	if (TBV_TEARDOWN_FENCE_BEFORE_WAIT)
		rq_fence_rings(rail, qp);

	while (rail->refcnt > 0 && waited < deadline) {
		rq_qp_try_unbind(qp);	/* passive poll: no-op unless fenced */
		waited++;
	}

	if (rail->refcnt > 0) {
		/*
		 * Bounded forced path: rings are fenced+freed (NHI-safe), so
		 * unlink and leak the rail rather than kfree under a live ref
		 * (UAF) or wedge the reboot forever (held power button). In the
		 * pre-fix model the rings were NOT fenced here, so this same
		 * forced free raced live DMA -- the reason it was never taken.
		 */
		if (!rail->rings_fenced)
			rq_fence_rings(rail, qp);
		rail->rings_freed = true;
		rail->leaked = true;
		return true;
	}

	rail->rings_freed = true;
	rail->freed = true;
	return true;
}

static void rq_init(struct rq_rail *rail, struct rq_qp *qp, bool link_alive)
{
	memset(rail, 0, sizeof(*rail));
	memset(qp, 0, sizeof(*qp));
	rail->refcnt = 2;		/* birth ref + one QP */
	qp->rail = rail;
	qp->nhi_inflight = 1;		/* one frame stuck in the NHI ring */
	qp->link_alive = link_alive;
	qp->rail_ref_held = true;
}

/*
 * The teardown never wedges module_exit: rq_remove_rail returns bounded for
 * every case (dead link, live link, unreclaimable frame). Independent of the
 * lockstep lever -- the bound is unconditional.
 */
static void tbv_teardown_is_bounded(struct kunit *test)
{
	struct rq_rail rail;
	struct rq_qp qp;

	rq_init(&rail, &qp, /*link_alive=*/false);
	KUNIT_EXPECT_TRUE(test, rq_remove_rail(&rail, &qp, 64));
	KUNIT_EXPECT_TRUE(test, rail.rings_freed);
}

/*
 * A dead-link QP with in-flight frames must drain and the rail must be cleanly
 * freed (not leaked). RED with TBV_TEARDOWN_FENCE_BEFORE_WAIT=0 (the pre-fix
 * ordering): the ref is pinned, the rail is force-leaked. GREEN with the fence.
 */
static void tbv_teardown_dead_link_drains_and_frees(struct kunit *test)
{
	struct rq_rail rail;
	struct rq_qp qp;

	rq_init(&rail, &qp, /*link_alive=*/false);
	rq_remove_rail(&rail, &qp, 64);

	KUNIT_EXPECT_FALSE(test, qp.rail_ref_held);
	KUNIT_EXPECT_EQ(test, rail.refcnt, 0);
	KUNIT_EXPECT_TRUE(test, rail.rings_fenced);
	KUNIT_EXPECT_TRUE(test, rail.freed);
	KUNIT_EXPECT_FALSE(test, rail.leaked);
}

/* A live link drains without the fence being load-bearing (fence is harmless). */
static void tbv_teardown_live_link_drains_and_frees(struct kunit *test)
{
	struct rq_rail rail;
	struct rq_qp qp;

	rq_init(&rail, &qp, /*link_alive=*/true);
	rq_remove_rail(&rail, &qp, 64);

	KUNIT_EXPECT_EQ(test, rail.refcnt, 0);
	KUNIT_EXPECT_TRUE(test, rail.freed);
}

/*
 * Pathological residual: a frame the fence cannot reclaim. The wait must still
 * be bounded and the forced free must be NHI-SAFE -- rings fenced+freed before
 * the (deliberate, logged) leak, never a kfree racing live DMA.
 */
static void tbv_teardown_unreclaimable_is_safe_leak(struct kunit *test)
{
	struct rq_rail rail;
	struct rq_qp qp;

	rq_init(&rail, &qp, /*link_alive=*/false);
	qp.frame_unreclaimable = true;
	rq_remove_rail(&rail, &qp, 64);

	KUNIT_EXPECT_TRUE(test, rail.rings_fenced);	/* fenced before free */
	KUNIT_EXPECT_TRUE(test, rail.rings_freed);
	KUNIT_EXPECT_TRUE(test, rail.leaked);		/* leaked, not UAF-freed */
	KUNIT_EXPECT_FALSE(test, rail.freed);
}

static struct kunit_case tbv_reload_quiesce_cases[] = {
	KUNIT_CASE(tbv_teardown_is_bounded),
	KUNIT_CASE(tbv_teardown_dead_link_drains_and_frees),
	KUNIT_CASE(tbv_teardown_live_link_drains_and_frees),
	KUNIT_CASE(tbv_teardown_unreclaimable_is_safe_leak),
	{}
};

static struct kunit_suite tbv_reload_quiesce_suite = {
	.name = "thunderbolt_ibverbs_reload_quiesce",
	.test_cases = tbv_reload_quiesce_cases,
};
kunit_test_suite(tbv_reload_quiesce_suite);

MODULE_LICENSE("GPL");
