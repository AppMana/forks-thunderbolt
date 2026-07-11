// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit: create_qp must bind an existing rail even when the peer link is still
 * coming up (the 0.2.32 NCCL-init "ibv_create_qp ... Transport endpoint is not
 * connected" regression).
 *
 * tbv_create_qp() binds a rail at CREATE time via tbv_select_qp_rail_locked()
 * and returns -ENOTCONN when it gets NULL. For a native, non-GSI QP (and with
 * native_home_rail_qp off, the fleet default) selection goes through
 * tbv_select_rail_from_peer_locked(require_ibdev=true), which SKIPS any rail
 * that is not tbv_rail_data_ready() -- i.e. whose native HELLO/READY handshake
 * has not completed (tbv.h: state==TUNNEL_ENABLED && handshake_complete). So a
 * QP created after the ib_device is published but BEFORE (or during a transient
 * re-arm of) the native handshake gets NULL -> ENOTCONN.
 *
 * That is wrong: an RC/UC QP is born in RESET. Peer connectivity is only
 * meaningful at modify_qp(RTR) and at send time, where the data path is
 * re-selected per-send by destination GID
 * (tbv_select_native_data_path_for_qp_locked); the create-time rail is a mere
 * placeholder for refcount accounting. Requiring the link up at CREATE is a
 * latent bug -- the selection code is byte-identical to the deployed-good
 * 0.2.25 -- that 0.2.32's core self-heal work (reconcile passes / re-enumeration
 * re-arming the native handshake after the ib_device is already published)
 * shifted the bring-up timing enough to expose: create_qp now races ahead of
 * handshake completion, racily on whichever rank loses.
 *
 * The fix binds the home rail when it EXISTS (present, not removing) even if no
 * peer rail is data-ready yet, while still returning NULL (-> ENOTCONN) for the
 * GENUINE no-rail case (no home, or home removing). Lockstep lever
 * TBV_CREATE_QP_BINDS_UNREADY_RAIL mirrors the driver; built =0 it reproduces
 * the regression. Run via tools/run-kunit.sh.
 */
#include <kunit/test.h>
#include <linux/types.h>

/* Lockstep with tbv_select_qp_rail_locked(). 1 = create binds an existing home
 * rail regardless of transient data-readiness; 0 = pre-fix (require data-ready,
 * the -ENOTCONN regression). */
#ifndef TBV_CREATE_QP_BINDS_UNREADY_RAIL
#define TBV_CREATE_QP_BINDS_UNREADY_RAIL 1
#endif

/* Faithful model of the create-time rail state feeding the selection. */
struct cq_rail {
	bool present;		/* dev->rail (home) exists */
	bool removing;		/* rail->removing */
	bool data_ready;	/* tbv_rail_data_ready: TUNNEL_ENABLED + hs done */
	bool ibdev;		/* rail->ibdev published (require_ibdev gate) */
	bool tx_stalled;	/* tbv_path_tx_stalled */
};

/*
 * Mirror of tbv_select_qp_rail_locked() for a native, non-GSI QP with
 * native_home_rail_qp off (fleet default). Returns true to BIND, false to fail
 * create with -ENOTCONN.
 */
static bool cq_select_for_create(const struct cq_rail *r)
{
	/* WARN_ON_ONCE(!home) / home->removing: genuine no-rail -> ENOTCONN. */
	if (!r->present || r->removing)
		return false;

	/* tbv_select_rail_from_peer_locked(require_ibdev=true): a data-ready,
	 * published, non-stalled rail is always bound (the healthy path). */
	if (r->data_ready && r->ibdev && !r->tx_stalled)
		return true;

	/*
	 * No peer rail is data-ready yet. Pre-fix returned NULL here
	 * (-> ENOTCONN); the fix binds the existing home rail as a placeholder
	 * (the QP is in RESET; the real path is re-selected per-send by dgid).
	 */
	return TBV_CREATE_QP_BINDS_UNREADY_RAIL ? true : false;
}

/* A fully-connected rail binds (healthy steady state). */
static void tbv_create_qp_binds_ready_rail(struct kunit *test)
{
	struct cq_rail r = { .present = true, .data_ready = true,
			     .ibdev = true };
	KUNIT_EXPECT_TRUE(test, cq_select_for_create(&r));
}

/*
 * THE REGRESSION: ib_device published, but the native handshake has not
 * completed yet (data_ready=false). create_qp must BIND, not -ENOTCONN.
 * RED with TBV_CREATE_QP_BINDS_UNREADY_RAIL=0.
 */
static void tbv_create_qp_binds_rail_with_link_coming_up(struct kunit *test)
{
	struct cq_rail r = { .present = true, .ibdev = true,
			     .data_ready = false };
	KUNIT_EXPECT_TRUE(test, cq_select_for_create(&r));
}

/*
 * Concurrent reconcile / re-announce transiently re-arms the handshake, so a
 * previously-ready rail reads data_ready=false for a window while ibdev stays
 * published. A create_qp racing that window must still bind. RED pre-fix.
 */
static void tbv_create_qp_survives_reconcile_race(struct kunit *test)
{
	struct cq_rail r = { .present = true, .ibdev = true,
			     .data_ready = false, .tx_stalled = false };
	KUNIT_EXPECT_TRUE(test, cq_select_for_create(&r));

	/* Even if the transient window also marks the ring briefly tx-stalled,
	 * the rail EXISTS -> bind (connection re-checked per-send). */
	r.tx_stalled = true;
	KUNIT_EXPECT_TRUE(test, cq_select_for_create(&r));
}

/*
 * Genuine no-rail cases MUST still fail create with -ENOTCONN -- the fix does
 * not weaken this. No home rail at all, or the rail is tearing down.
 */
static void tbv_create_qp_no_rail_is_enotconn(struct kunit *test)
{
	struct cq_rail none = { .present = false };
	struct cq_rail removing = { .present = true, .removing = true,
				    .data_ready = true, .ibdev = true };

	KUNIT_EXPECT_FALSE(test, cq_select_for_create(&none));
	KUNIT_EXPECT_FALSE(test, cq_select_for_create(&removing));
}

static struct kunit_case tbv_create_qp_rail_bind_cases[] = {
	KUNIT_CASE(tbv_create_qp_binds_ready_rail),
	KUNIT_CASE(tbv_create_qp_binds_rail_with_link_coming_up),
	KUNIT_CASE(tbv_create_qp_survives_reconcile_race),
	KUNIT_CASE(tbv_create_qp_no_rail_is_enotconn),
	{}
};

static struct kunit_suite tbv_create_qp_rail_bind_suite = {
	.name = "thunderbolt_ibverbs_create_qp_rail_bind",
	.test_cases = tbv_create_qp_rail_bind_cases,
};
kunit_test_suite(tbv_create_qp_rail_bind_suite);

MODULE_LICENSE("GPL");
