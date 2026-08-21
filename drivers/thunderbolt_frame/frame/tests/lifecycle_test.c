// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit: module/client lifecycle contracts -- the ones that decide whether
 * thunderbolt_frame.ko can be unloaded and reloaded repeatedly instead of costing a
 * reboot per test iteration.
 *
 * Every case here is a regression for a defect found by auditing tbframe
 * against the in-tree Thunderbolt service drivers (tbnet, dma_test) and the
 * legacy tbv peer.c teardown; see the pattern catalog in the commit series.
 *
 * The inbound-dispatch cases matter because the core runs a protocol handler
 * callback from the control channel's RX ring work item with the global
 * xdomain_dispatch_lock held (drivers/thunderbolt/xdomain.c, commit 054b92c),
 * i.e. exactly where a peer that keeps HELLOing through our teardown lands.
 */
#include "tbframe_mock.h"

/* Feed the core an inbound peer message as the dispatch path would. */
static int tbframe_lifecycle_inject(struct tbframe_mock_fixture *fx, u16 op)
{
	u8 msg[TBFRAME_WIRE_HELLO_MSG_SIZE];
	int ret;

	ret = tbframe_mock_build_peer_msg(fx, op, msg, sizeof(msg));
	if (ret < 0)
		return ret;
	/* token NULL: exercise the source-blind walk, the widest path. */
	return tbframe_handle_packet(&fx->tf, NULL, msg, sizeof(msg));
}

/*
 * An inbound HELLO must never be answered from a link that is being torn
 * down. Answering mutates session state (remote HopID, handshake, retry
 * budgets) behind a teardown that has already snapshotted it.
 */
static void tbframe_lifecycle_dispatch_refused_while_removing(struct kunit *test)
{
	struct tbframe_mock_fixture *fx = test->priv;
	unsigned long flags;
	int consumed;

	tbframe_mock_link_up(test, fx);
	fx->mock.have_response = false;

	/* Sanity: a healthy link does answer. */
	consumed = tbframe_lifecycle_inject(fx, TBFRAME_WIRE_OP_HELLO);
	KUNIT_ASSERT_EQ(test, 1, consumed);
	KUNIT_ASSERT_TRUE(test, fx->mock.have_response);

	/* Now simulate teardown having claimed the link. */
	spin_lock_irqsave(&fx->link->lock, flags);
	fx->link->removing = true;
	spin_unlock_irqrestore(&fx->link->lock, flags);
	fx->mock.have_response = false;

	consumed = tbframe_lifecycle_inject(fx, TBFRAME_WIRE_OP_HELLO);
	KUNIT_EXPECT_EQ(test, 0, consumed);
	KUNIT_EXPECT_FALSE(test, fx->mock.have_response);

	spin_lock_irqsave(&fx->link->lock, flags);
	fx->link->removing = false;
	spin_unlock_irqrestore(&fx->link->lock, flags);
}

/*
 * tbframe_link_destroy() must complete while inbound dispatch is hammering
 * the same link, and must leave nothing behind that a later dispatch can
 * reach. The reference the dispatch walk takes is covered by destroy's
 * bounded refs-zero wait, so this returns 0 (fully freed), not -EBUSY.
 */
static void tbframe_lifecycle_destroy_under_dispatch(struct kunit *test)
{
	struct tbframe_mock_fixture *fx = test->priv;
	int i, ret;

	tbframe_mock_link_up(test, fx);

	for (i = 0; i < 8; i++)
		tbframe_lifecycle_inject(fx, TBFRAME_WIRE_OP_HELLO);

	ret = tbframe_link_destroy(fx->link, TBFRAME_DOWN_UNPLUG);
	fx->link_destroyed = true;
	KUNIT_EXPECT_EQ(test, 0, ret);

	/* The link is off the list: dispatch finds nothing and consumes nothing. */
	KUNIT_EXPECT_EQ(test, 0, tbframe_lifecycle_inject(fx,
							  TBFRAME_WIRE_OP_HELLO));
	KUNIT_EXPECT_TRUE(test, list_empty(&fx->tf.links));
}

/*
 * link_down exactly once per up link at client unregister, and it is the
 * LAST upcall. A peer mid-handshake keeps sending HELLO/READY through the
 * unregister; without the park gate those re-arm the session work, drive the
 * link back UP and deliver a link_up() the client can never match with a
 * link_down().
 */
static void tbframe_lifecycle_link_down_once_at_unregister(struct kunit *test)
{
	struct tbframe_mock_fixture *fx = test->priv;

	tbframe_mock_link_up(test, fx);
	KUNIT_ASSERT_EQ(test, 1u, fx->client.up_count);
	KUNIT_ASSERT_EQ(test, 0u, fx->client.down_count);

	tbframe_unregister_client_tf(&fx->tf);

	KUNIT_EXPECT_EQ(test, 1u, fx->client.down_count);
	KUNIT_EXPECT_EQ(test, (int)TBFRAME_DOWN_CLOSED,
			fx->client.reasons[fx->client.count - 1]);
	KUNIT_EXPECT_EQ(test, 1u, fx->client.up_count);

	/* A peer still negotiating must not resurrect the parked session. */
	tbframe_lifecycle_inject(fx, TBFRAME_WIRE_OP_HELLO);
	tbframe_lifecycle_inject(fx, TBFRAME_WIRE_OP_READY);
	flush_workqueue(fx->tf.wq);
	tbframe_link_session_step(fx->link);
	flush_workqueue(fx->tf.wq);

	KUNIT_EXPECT_EQ(test, 1u, fx->client.up_count);
	KUNIT_EXPECT_EQ(test, 1u, fx->client.down_count);
	KUNIT_EXPECT_EQ(test, (int)TBFRAME_STATE_INIT, (int)fx->link->state);
}

/*
 * Re-register immediately after unregister: the parked session must unpark
 * and reach UP again. This is the module-reload shape of the client (tbrxe
 * rmmod/insmod) with tbframe staying loaded.
 */
static void tbframe_lifecycle_reregister_after_unregister(struct kunit *test)
{
	struct tbframe_mock_fixture *fx = test->priv;
	int ret;

	tbframe_mock_link_up(test, fx);
	tbframe_unregister_client_tf(&fx->tf);
	KUNIT_ASSERT_EQ(test, 1u, fx->client.down_count);
	KUNIT_ASSERT_TRUE(test, fx->link->parked);

	ret = tbframe_register_client_tf(&fx->tf, &tbframe_mock_client_ops,
					 &fx->client);
	KUNIT_ASSERT_EQ(test, 0, ret);
	KUNIT_EXPECT_FALSE(test, fx->link->parked);

	flush_workqueue(fx->tf.wq);
	tbframe_link_session_step(fx->link);

	KUNIT_EXPECT_EQ(test, 2u, fx->client.up_count);
	KUNIT_EXPECT_EQ(test, (int)TBFRAME_STATE_UP, (int)fx->link->state);
}

/* Registering twice is refused; unregister/register cycles stay balanced. */
static void tbframe_lifecycle_double_register_refused(struct kunit *test)
{
	struct tbframe_mock_fixture *fx = test->priv;

	KUNIT_EXPECT_EQ(test, -EBUSY,
			tbframe_register_client_tf(&fx->tf,
						   &tbframe_mock_client_ops,
						   &fx->client));
	tbframe_unregister_client_tf(&fx->tf);
	KUNIT_EXPECT_EQ(test, 0,
			tbframe_register_client_tf(&fx->tf,
						   &tbframe_mock_client_ops,
						   &fx->client));
}

/*
 * Teardown must release the in-HopID it ACTUALLY allocated, not whatever the
 * peer's most recent HELLO left in ->remote_hopid. An inbound HELLO runs from
 * the dispatch context with no session lock, so it can move that field at any
 * time; releasing the moved value both splats ida_free() in the core and
 * leaks the real HopID, after which the next session's alloc fails -EBUSY and
 * the link never comes back across a reload.
 */
static void tbframe_lifecycle_hopid_release_matches_alloc(struct kunit *test)
{
	struct tbframe_mock_fixture *fx = test->priv;
	int allocated;

	tbframe_mock_link_up(test, fx);
	allocated = fx->mock.last_alloc_in_hopid;
	KUNIT_ASSERT_EQ(test, 1u, fx->mock.in_hopid_allocs);
	KUNIT_ASSERT_EQ(test, (int)fx->mock.peer.transmit_hopid, allocated);

	/* Peer re-HELLOs from a different transmit HopID while we are up. */
	fx->mock.peer.transmit_hopid = allocated + 5;
	tbframe_lifecycle_inject(fx, TBFRAME_WIRE_OP_HELLO);
	KUNIT_ASSERT_EQ(test, (int)fx->mock.peer.transmit_hopid,
			(int)fx->link->remote_hopid);

	tbframe_link_destroy(fx->link, TBFRAME_DOWN_UNPLUG);
	fx->link_destroyed = true;

	KUNIT_EXPECT_EQ(test, 1u, fx->mock.in_hopid_releases);
	KUNIT_EXPECT_EQ(test, allocated, fx->mock.last_release_in_hopid);
	KUNIT_EXPECT_EQ(test, allocated, fx->mock.last_disable_remote_hopid);
}

/*
 * A parked link does not negotiate. The dispatch fence used to cover only
 * removing/DEAD, so a parked link's dispatch still answered HELLO: during
 * a client reload the peer completed a one-sided bring_up against the
 * parked session and its freshly-programmed path endpoint sat unmatched
 * for the whole teardown budget -- the eventually-paired session came up
 * born dead at the router level (v3 storm cycle 1). Refuse HELLO/READY
 * while parked or mid-teardown; the peer's retry lands once we're back.
 */
static void tbframe_lifecycle_parked_refuses_negotiation(struct kunit *test)
{
	struct tbframe_mock_fixture *fx = test->priv;

	tbframe_mock_link_up(test, fx);
	tbframe_unregister_client_tf(&fx->tf);
	KUNIT_ASSERT_TRUE(test, fx->link->parked);
	fx->mock.have_response = false;

	KUNIT_EXPECT_EQ(test, 0,
			tbframe_lifecycle_inject(fx, TBFRAME_WIRE_OP_HELLO));
	KUNIT_EXPECT_EQ(test, 0,
			tbframe_lifecycle_inject(fx, TBFRAME_WIRE_OP_READY));
	KUNIT_EXPECT_FALSE(test, fx->mock.have_response);

	/* Unparked, negotiation resumes normally. */
	KUNIT_ASSERT_EQ(test, 0,
			tbframe_register_client_tf(&fx->tf,
						   &tbframe_mock_client_ops,
						   &fx->client));
	KUNIT_EXPECT_EQ(test, 1,
			tbframe_lifecycle_inject(fx, TBFRAME_WIRE_OP_HELLO));
	KUNIT_EXPECT_TRUE(test, fx->mock.have_response);
}

/*
 * The transmit HopID is stable across sessions -- pinned deliberately.
 *
 * Per-session rotation was tried and measured on the 023/025 canaries
 * (2026-08-18): it does not recover the router egress wedge (per-port
 * poison, reset-only) and it destabilizes re-negotiation -- a peer that
 * rotates in its down_session lands the new HopID after this side's
 * bring_up snapshot, yielding a stable-but-dead session with the ingress
 * hop on the stale id. Static ids make every re-negotiation converge, so
 * a session cycle must advertise the same HopID and neither allocate nor
 * release anything; the single release belongs to destroy.
 */
static void tbframe_lifecycle_out_hopid_stable_across_sessions(struct kunit *test)
{
	struct tbframe_mock_fixture *fx = test->priv;
	int first;

	tbframe_mock_link_up(test, fx);
	first = fx->mock.last_request_tx_hopid;
	KUNIT_ASSERT_EQ(test, fx->link->local_hopid, first);
	KUNIT_ASSERT_EQ(test, 1u, fx->mock.out_hopid_allocs);

	/* Peer reboots and re-HELLOs: supersede, full re-handshake. */
	fx->mock.peer.session_cookie ^= 0xffull;
	tbframe_lifecycle_inject(fx, TBFRAME_WIRE_OP_HELLO);
	flush_workqueue(fx->tf.wq);
	tbframe_link_session_step(fx->link);
	flush_workqueue(fx->tf.wq);
	KUNIT_ASSERT_EQ(test, 2u, fx->client.up_count);

	KUNIT_EXPECT_EQ(test, first, fx->mock.last_request_tx_hopid);
	KUNIT_EXPECT_EQ(test, first, fx->link->local_hopid);
	KUNIT_EXPECT_EQ(test, 1u, fx->mock.out_hopid_allocs);
	KUNIT_EXPECT_EQ(test, 0u, fx->mock.out_hopid_releases);

	tbframe_link_destroy(fx->link, TBFRAME_DOWN_UNPLUG);
	fx->link_destroyed = true;
	KUNIT_EXPECT_EQ(test, 1u, fx->mock.out_hopid_releases);
	KUNIT_EXPECT_EQ(test, first, fx->mock.last_release_out_hopid);
}

/*
 * Destroy against hardware that never cancels a frame AND an inbound peer
 * still hammering the link: still bounded, still no hang.
 */
static void tbframe_lifecycle_destroy_dead_hw_under_dispatch(struct kunit *test)
{
	struct tbframe_mock_fixture *fx = test->priv;
	unsigned long start;
	int i, ret;

	tbframe_mock_link_up(test, fx);
	fx->mock.never_cancel = true;

	for (i = 0; i < 4; i++)
		tbframe_lifecycle_inject(fx, TBFRAME_WIRE_OP_HELLO);

	start = jiffies;
	ret = tbframe_link_destroy(fx->link, TBFRAME_DOWN_UNPLUG);
	fx->link_destroyed = true;

	KUNIT_EXPECT_EQ(test, -EBUSY, ret);
	KUNIT_EXPECT_LT(test, jiffies_to_msecs(jiffies - start), 5000u);
	KUNIT_EXPECT_EQ(test, (int)TBFRAME_STATE_DEAD, (int)fx->link->state);
	/* Delisted even on the forced-leak path: no dispatch can reach it. */
	KUNIT_EXPECT_TRUE(test, list_empty(&fx->tf.links));
	KUNIT_EXPECT_EQ(test, 0, tbframe_lifecycle_inject(fx,
							  TBFRAME_WIRE_OP_HELLO));
}

static int tbframe_lifecycle_test_init(struct kunit *test)
{
	struct tbframe_mock_fixture *fx;
	int ret;

	fx = kunit_kzalloc(test, sizeof(*fx), GFP_KERNEL);
	if (!fx)
		return -ENOMEM;
	ret = tbframe_mock_fixture_init(test, fx);
	if (ret)
		return ret;
	test->priv = fx;
	return 0;
}

static void tbframe_lifecycle_test_exit(struct kunit *test)
{
	tbframe_mock_fixture_exit(test, test->priv);
}

static struct kunit_case tbframe_lifecycle_cases[] = {
	KUNIT_CASE(tbframe_lifecycle_dispatch_refused_while_removing),
	KUNIT_CASE(tbframe_lifecycle_destroy_under_dispatch),
	KUNIT_CASE(tbframe_lifecycle_link_down_once_at_unregister),
	KUNIT_CASE(tbframe_lifecycle_reregister_after_unregister),
	KUNIT_CASE(tbframe_lifecycle_double_register_refused),
	KUNIT_CASE(tbframe_lifecycle_hopid_release_matches_alloc),
	KUNIT_CASE(tbframe_lifecycle_parked_refuses_negotiation),
	KUNIT_CASE(tbframe_lifecycle_out_hopid_stable_across_sessions),
	KUNIT_CASE(tbframe_lifecycle_destroy_dead_hw_under_dispatch),
	{}
};

static struct kunit_suite tbframe_lifecycle_suite = {
	.name = "tbframe_lifecycle",
	.init = tbframe_lifecycle_test_init,
	.exit = tbframe_lifecycle_test_exit,
	.test_cases = tbframe_lifecycle_cases,
};
kunit_test_suite(tbframe_lifecycle_suite);

MODULE_LICENSE("GPL");
