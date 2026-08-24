// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit model of thunderbolt_net's UNBIND, SHUTDOWN and SUSPEND teardown
 * ordering.
 *
 * Incident being closed, appmana-019 2026-08-24: on a Maple Ridge AM5 chain
 * node with both chain links live and no workload,
 *
 *   modprobe -r thunderbolt_frame_rxe && modprobe -r thunderbolt_frame &&
 *   modprobe -r thunderbolt_net && modprobe -r thunderbolt
 *
 * hung the node hard -- no ssh, no ping, physical power cycle required. The
 * same sequence on appmana-021 earlier the same day completed cleanly, so it
 * is a race, not a deterministic failure. appmana-008 the same day hung on a
 * plain `systemctl reboot` after a leaf-only reload, with its 019-facing
 * XDomain in the failed/retrying state and the peer HELLO-ing at it endlessly
 * -- the same teardown, reached through device_shutdown() instead of unbind.
 *
 * Two properties are modelled, because a real deadlock or a real
 * use-after-free cannot be made to manifest deterministically in a unit test:
 *
 *  1. Ordering as OBSERVABLE STATE. The model records lock acquisitions in a
 *     ledger, so an inversion is an assertion failure rather than something
 *     that has to actually deadlock to be noticed. The unload path's order is
 *     xdomain_dispatch_lock -> RTNL (tb_unregister_protocol_handler() takes
 *     the dispatch lock, which tb_xdomain_handle_request() holds across the
 *     handler callbacks; unregister_netdev() then takes RTNL). Acquiring the
 *     dispatch lock while holding RTNL closes the cycle with the ctl RX
 *     worker and is counted as an inversion here.
 *
 *  2. Use-after-free as a COUNTER. The model frees the netdev at the end and
 *     counts any work item that still runs afterwards. In the real driver
 *     those handlers are tbnet_connected_work() (tb_ring_start() on freed
 *     ring pointers) and tbnet_disconnect_work() -> tbnet_tear_down()
 *     (tb_ring_stop(), tb_xdomain_disable_paths()), i.e. NHI MMIO writes
 *     derived from freed struct tbnet state -- which presents as an
 *     unrecoverable machine hang, not a clean oops.
 *
 * RED-first levers, matching main.c:
 *   TBNET_UNBIND_FENCE_FIRST  1 = ship (tbnet_fence_handler() before
 *                                 unregister_netdev()/tear_down),
 *                             0 = pre-2.46 (handler unregistered last, and
 *                                 tbnet_shutdown()/tbnet_suspend() never
 *                                 fenced at all).
 *   TBNET_UNBIND_FINAL_CANCEL 1 = ship (tbnet_cancel_all_work() after the
 *                                 fence and before free_netdev()),
 *                             0 = pre-2.46 (only tbnet_stop()'s two cancels,
 *                                 taken at the START of a multi-second
 *                                 RTNL-held window).
 * Both at 0 reproduces the appmana-019 shape as test failures.
 *
 * Built into the module only on a CONFIG_KUNIT kernel; run via
 * drivers/thunderbolt_frame/tools/run-kunit.sh.
 */
#include <kunit/test.h>
#include <linux/kernel.h>

#ifndef TBNET_UNBIND_FENCE_FIRST
#define TBNET_UNBIND_FENCE_FIRST 1
#endif
#ifndef TBNET_UNBIND_FINAL_CANCEL
#define TBNET_UNBIND_FINAL_CANCEL 1
#endif

/* main.c constants, mirrored so the bound assertions stay in lockstep. */
#define MODEL_LOGOUT_RETRIES	10
#define MODEL_LOGOUT_TIMEOUT	1000
#define MODEL_LOGOUT_BUDGET_MS	3000

enum tbnet_model_lock {
	MODEL_LOCK_DISPATCH = 1,	/* xdomain_dispatch_lock */
	MODEL_LOCK_RTNL,
};

/* Work items struct tbnet owns; a set bit means "queued and not cancelled". */
#define MODEL_W_LOGIN		BIT(0)
#define MODEL_W_CONNECTED	BIT(1)
#define MODEL_W_VERIFY		BIT(2)
#define MODEL_W_DISCONNECT	BIT(3)
#define MODEL_W_ALL		(MODEL_W_LOGIN | MODEL_W_CONNECTED | \
				 MODEL_W_VERIFY | MODEL_W_DISCONNECT)

struct tbnet_unbind_model {
	bool		handler_registered;
	bool		netdev_registered;
	bool		netdev_up;
	bool		freed;
	bool		paths_enabled;
	bool		rings_started;

	unsigned int	queued;
	/* work that ran (or would have run) after free_netdev() */
	unsigned int	uaf_runs;
	/* data path re-armed after the teardown had taken it down */
	unsigned int	rearms_after_teardown;

	/* lock ledger */
	u8		held[4];
	unsigned int	depth;
	unsigned int	inversions;

	/* peer-wait accounting */
	bool		peer_answers;
	unsigned int	logout_attempts;
	unsigned int	peer_wait_ms;
	unsigned int	rtnl_held_ms;
};

static void model_lock(struct kunit *test, struct tbnet_unbind_model *m, u8 l)
{
	unsigned int i;

	/*
	 * The inversion that matters: taking the XDomain dispatch lock while
	 * holding RTNL. The ctl RX worker holds the dispatch lock across the
	 * protocol-handler callbacks, so a path in that order can wait on a
	 * worker that is waiting for RTNL.
	 */
	if (l == MODEL_LOCK_DISPATCH)
		for (i = 0; i < m->depth; i++)
			if (m->held[i] == MODEL_LOCK_RTNL)
				m->inversions++;

	KUNIT_ASSERT_LT(test, m->depth, ARRAY_SIZE(m->held));
	m->held[m->depth++] = l;
}

static void model_unlock(struct kunit *test, struct tbnet_unbind_model *m, u8 l)
{
	KUNIT_ASSERT_GT(test, m->depth, 0u);
	KUNIT_ASSERT_EQ(test, (int)m->held[m->depth - 1], (int)l);
	m->depth--;
}

static bool model_holds(const struct tbnet_unbind_model *m, u8 l)
{
	unsigned int i;

	for (i = 0; i < m->depth; i++)
		if (m->held[i] == l)
			return true;
	return false;
}

/* --- modelled driver entry points, mirroring main.c ------------------- */

/* tb_unregister_protocol_handler(): takes the dispatch lock, idempotent. */
static void model_fence_handler(struct kunit *test,
				struct tbnet_unbind_model *m)
{
	if (!m->handler_registered)
		return;
	m->handler_registered = false;
	model_lock(test, m, MODEL_LOCK_DISPATCH);
	model_unlock(test, m, MODEL_LOCK_DISPATCH);
}

/*
 * tbnet_handle_packet(): a peer LOGIN or LOGOUT arriving from the XDomain
 * dispatch context. Runs with the dispatch lock held by the caller, and
 * queues work on system_long_wq.
 */
static void model_peer_packet(struct kunit *test, struct tbnet_unbind_model *m)
{
	if (!m->handler_registered)
		return;		/* structurally excluded by the fence */

	model_lock(test, m, MODEL_LOCK_DISPATCH);
	m->queued |= MODEL_W_LOGIN | MODEL_W_CONNECTED | MODEL_W_DISCONNECT;
	model_unlock(test, m, MODEL_LOCK_DISPATCH);
}

/* tbnet_logout_request() loop inside tbnet_tear_down(). */
static void model_logout(struct kunit *test, struct tbnet_unbind_model *m)
{
	unsigned int retries = MODEL_LOGOUT_RETRIES;

	while (retries--) {
		m->logout_attempts++;
		if (m->peer_answers)
			return;
		m->peer_wait_ms += MODEL_LOGOUT_TIMEOUT;
		if (m->rtnl_held_ms || model_holds(m, MODEL_LOCK_RTNL))
			m->rtnl_held_ms += MODEL_LOGOUT_TIMEOUT;
		/* Hard deadline on top of the retry count (2.46). */
		if (m->peer_wait_ms >= MODEL_LOGOUT_BUDGET_MS)
			return;
	}
}

static void model_tear_down(struct kunit *test, struct tbnet_unbind_model *m)
{
	/* stop_login() + the verify cancel at the head of tbnet_tear_down() */
	m->queued &= ~(MODEL_W_LOGIN | MODEL_W_CONNECTED | MODEL_W_VERIFY);
	model_logout(test, m);
	m->rings_started = false;
	m->paths_enabled = false;
}

/* tbnet_stop(), i.e. ndo_stop, called by unregister_netdev() under RTNL. */
static void model_ndo_stop(struct kunit *test, struct tbnet_unbind_model *m)
{
	m->queued &= ~(MODEL_W_VERIFY | MODEL_W_DISCONNECT);
	if (m->rtnl_held_ms == 0)
		m->rtnl_held_ms = 1;	/* mark the window as open */
	model_tear_down(test, m);
}

/*
 * unregister_netdev(). Takes RTNL for the whole call, including ndo_stop. A
 * peer packet delivered during that window is the race: it is delivered from
 * a different context (the ctl RX worker), so it is injected here.
 */
static void model_unregister_netdev(struct kunit *test,
				    struct tbnet_unbind_model *m,
				    bool peer_active)
{
	model_lock(test, m, MODEL_LOCK_RTNL);
	if (m->netdev_up)
		model_ndo_stop(test, m);
	if (peer_active)
		model_peer_packet(test, m);
	m->netdev_registered = false;
	model_unlock(test, m, MODEL_LOCK_RTNL);
}

static void model_cancel_all_work(struct tbnet_unbind_model *m)
{
	if (!TBNET_UNBIND_FINAL_CANCEL)
		return;
	m->queued &= ~MODEL_W_ALL;
}

/* Drain whatever is still queued, after the netdev has been freed. */
static void model_drain(struct tbnet_unbind_model *m)
{
	if (!m->queued)
		return;
	if (m->freed) {
		m->uaf_runs += hweight32(m->queued);
	} else if (m->queued & MODEL_W_CONNECTED) {
		/* tbnet_connected_work(): tb_ring_start + enable_paths */
		m->rings_started = true;
		m->paths_enabled = true;
		m->rearms_after_teardown++;
	}
	m->queued = 0;
}

static void model_remove(struct kunit *test, struct tbnet_unbind_model *m,
			 bool peer_active)
{
	if (TBNET_UNBIND_FENCE_FIRST)
		model_fence_handler(test, m);

	model_unregister_netdev(test, m, peer_active);

	if (!TBNET_UNBIND_FENCE_FIRST)
		model_fence_handler(test, m);

	model_cancel_all_work(m);
	m->freed = true;
	model_drain(m);
}

static void model_shutdown(struct kunit *test, struct tbnet_unbind_model *m,
			   bool peer_active)
{
	if (TBNET_UNBIND_FENCE_FIRST)
		model_fence_handler(test, m);

	model_tear_down(test, m);
	if (peer_active)
		model_peer_packet(test, m);

	model_cancel_all_work(m);
	/* device_shutdown() does not free; the reboot follows. */
	model_drain(m);
}

static void model_init(struct tbnet_unbind_model *m)
{
	memset(m, 0, sizeof(*m));
	m->handler_registered = true;
	m->netdev_registered = true;
	m->netdev_up = true;
	m->paths_enabled = true;
	m->rings_started = true;
}

/* --- tests ------------------------------------------------------------ */

/*
 * (a)+(b) Unbind racing an actively HELLO-ing peer. Nothing may run after the
 * netdev is freed, and the dispatch lock must never be taken under RTNL.
 */
static void tbnet_unbind_no_work_after_free(struct kunit *test)
{
	struct tbnet_unbind_model m;

	model_init(&m);
	model_remove(test, &m, true);

	KUNIT_EXPECT_EQ(test, 0u, m.uaf_runs);
	KUNIT_EXPECT_EQ(test, 0u, m.queued);
	KUNIT_EXPECT_EQ(test, 0u, m.inversions);
	KUNIT_EXPECT_EQ(test, 0u, m.depth);
}

/*
 * The fence is what makes the cancel meaningful: once the handler is gone the
 * peer cannot queue anything, so the ordering itself is the guarantee rather
 * than a race that the cancel happens to win.
 */
static void tbnet_unbind_fence_excludes_peer(struct kunit *test)
{
	struct tbnet_unbind_model m;

	model_init(&m);
	model_fence_handler(test, &m);

	/* The peer keeps trying; nothing can be queued any more. */
	model_peer_packet(test, &m);
	model_peer_packet(test, &m);
	KUNIT_EXPECT_EQ(test, 0u, m.queued);
	KUNIT_EXPECT_FALSE(test, m.handler_registered);

	/* ...and the fence is idempotent, because shutdown runs before unbind. */
	model_fence_handler(test, &m);
	KUNIT_EXPECT_EQ(test, 0u, m.depth);
}

/*
 * (b) Lock-acquisition ORDER as observable state. The unbind path must
 * establish dispatch-before-RTNL, never the reverse.
 */
static void tbnet_unbind_no_rtnl_dispatch_inversion(struct kunit *test)
{
	struct tbnet_unbind_model m;

	model_init(&m);
	model_remove(test, &m, true);
	KUNIT_EXPECT_EQ(test, 0u, m.inversions);

	model_init(&m);
	model_shutdown(test, &m, true);
	KUNIT_EXPECT_EQ(test, 0u, m.inversions);
}

/*
 * (c) A peer that never answers. The logout wait must be bounded by the hard
 * deadline, not merely by the retry count, and the teardown must complete.
 * The bound matters because this runs under RTNL from ndo_stop (stalling
 * every rtnl_lock() user on the box) and once per link from
 * device_shutdown().
 */
static void tbnet_unbind_silent_peer_is_bounded(struct kunit *test)
{
	struct tbnet_unbind_model m;

	model_init(&m);
	m.peer_answers = false;
	model_remove(test, &m, false);

	KUNIT_EXPECT_LE(test, m.peer_wait_ms, (unsigned int)MODEL_LOGOUT_BUDGET_MS);
	KUNIT_EXPECT_LT(test, m.logout_attempts, (unsigned int)MODEL_LOGOUT_RETRIES);
	/* Bounded AND complete: the local teardown still ran. */
	KUNIT_EXPECT_FALSE(test, m.paths_enabled);
	KUNIT_EXPECT_FALSE(test, m.rings_started);
	KUNIT_EXPECT_EQ(test, 0u, m.uaf_runs);
}

/* A live peer costs one round trip and no budget at all. */
static void tbnet_unbind_live_peer_is_cheap(struct kunit *test)
{
	struct tbnet_unbind_model m;

	model_init(&m);
	m.peer_answers = true;
	model_remove(test, &m, false);

	KUNIT_EXPECT_EQ(test, 1u, m.logout_attempts);
	KUNIT_EXPECT_EQ(test, 0u, m.peer_wait_ms);
}

/*
 * appmana-008's shape: shutdown with the peer HELLO-ing endlessly. The
 * teardown must be MONOTONIC -- nothing may put the rings or the DMA paths
 * back after they have been taken down, because nhi_shutdown() is next and
 * pcim devres unmaps the BAR right after it.
 */
static void tbnet_shutdown_is_monotonic_under_peer_hello(struct kunit *test)
{
	struct tbnet_unbind_model m;

	model_init(&m);
	m.peer_answers = false;		/* peer was powered off, then came back */
	model_shutdown(test, &m, true);

	KUNIT_EXPECT_EQ(test, 0u, m.rearms_after_teardown);
	KUNIT_EXPECT_FALSE(test, m.rings_started);
	KUNIT_EXPECT_FALSE(test, m.paths_enabled);
	KUNIT_EXPECT_EQ(test, 0u, m.queued);
	KUNIT_EXPECT_LE(test, m.peer_wait_ms, (unsigned int)MODEL_LOGOUT_BUDGET_MS);
}

/*
 * A netdev that was never brought up still gets its handler fenced and its
 * work cancelled: tbnet_handle_packet() queues disconnect_work for an inbound
 * LOGOUT with no netif_running() guard, so "interface was down" is not a
 * reason for the teardown to skip the fence.
 */
static void tbnet_unbind_down_interface_still_fenced(struct kunit *test)
{
	struct tbnet_unbind_model m;

	model_init(&m);
	m.netdev_up = false;
	model_remove(test, &m, true);

	KUNIT_EXPECT_EQ(test, 0u, m.uaf_runs);
	KUNIT_EXPECT_EQ(test, 0u, m.inversions);
}

static struct kunit_case tbnet_unbind_cases[] = {
	KUNIT_CASE(tbnet_unbind_no_work_after_free),
	KUNIT_CASE(tbnet_unbind_fence_excludes_peer),
	KUNIT_CASE(tbnet_unbind_no_rtnl_dispatch_inversion),
	KUNIT_CASE(tbnet_unbind_silent_peer_is_bounded),
	KUNIT_CASE(tbnet_unbind_live_peer_is_cheap),
	KUNIT_CASE(tbnet_shutdown_is_monotonic_under_peer_hello),
	KUNIT_CASE(tbnet_unbind_down_interface_still_fenced),
	{}
};

static struct kunit_suite tbnet_unbind_test_suite = {
	.name = "thunderbolt_net_unbind",
	.test_cases = tbnet_unbind_cases,
};
kunit_test_suite(tbnet_unbind_test_suite);
