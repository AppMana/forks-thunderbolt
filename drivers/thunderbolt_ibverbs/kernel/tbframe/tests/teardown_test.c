// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit: bounded teardown against dead hardware.
 *
 * Two invariants from the contract header: every internal hardware wait is
 * bounded and a timeout poisons the link to TBFRAME_DOWN_DEAD_HW (never
 * blocks the caller forever), and teardown completes against hardware that
 * never cancels its frames by a bounded refs wait plus a deliberate leak
 * (the legacy peer.c discipline). Run via tools/run-kunit.sh.
 */
#include "tbframe_mock.h"

/*
 * Session teardown quiesces the NHI before touching the fabric: rings stop
 * (all DMA cancelled) BEFORE the hop entries are disabled, the
 * tbnet_tear_down() order. Disabling the egress hop entry under a still
 * actively-DMAing TX ring is the local half of the "rapid disable of an
 * active path" pattern implicated in the router-level egress wedge.
 */
static void tbframe_teardown_stops_rings_before_paths(struct kunit *test)
{
	struct tbframe_mock_fixture *fx = test->priv;
	unsigned long flags;
	int stop, disable, freed, release;

	tbframe_mock_link_up(test, fx);

	spin_lock_irqsave(&fx->link->lock, flags);
	fx->link->needs_down = true;
	fx->link->down_reason = TBFRAME_DOWN_VERIFY;
	spin_unlock_irqrestore(&fx->link->lock, flags);
	tbframe_link_session_step(fx->link);

	stop = tbframe_mock_hw_call_pos(&fx->mock, TBFRAME_HW_STOP_RINGS, 0);
	disable = tbframe_mock_hw_call_pos(&fx->mock, TBFRAME_HW_DISABLE_PATHS,
					   0);
	freed = tbframe_mock_hw_call_pos(&fx->mock, TBFRAME_HW_FREE_RINGS, 0);
	release = tbframe_mock_hw_call_pos(&fx->mock,
					   TBFRAME_HW_RELEASE_IN_HOPID, 0);
	KUNIT_ASSERT_GE(test, stop, 0);
	KUNIT_ASSERT_GE(test, disable, 0);
	KUNIT_ASSERT_GE(test, freed, 0);
	KUNIT_ASSERT_GE(test, release, 0);
	/* stop -> disable -> free -> release */
	KUNIT_EXPECT_LT(test, stop, disable);
	KUNIT_EXPECT_LT(test, disable, freed);
	KUNIT_EXPECT_LT(test, freed, release);
}

/*
 * Orderly teardown must quiesce the peer FIRST: a BYE goes out while this
 * side's rings are still up (so the peer's in-flight frames still land in
 * real descriptors), and only then does the hardware teardown run. Without
 * it the peer keeps streaming into hop entries this side is disabling, and
 * its router egress wedges persistently (canary validation cycle 6:
 * surviving transmitter frozen at zero TX consumption, reboot-only).
 */
static void tbframe_teardown_sends_bye_while_rings_up(struct kunit *test)
{
	struct tbframe_mock_fixture *fx = test->priv;
	unsigned long flags;

	tbframe_mock_link_up(test, fx);
	KUNIT_ASSERT_EQ(test, 0u, fx->mock.bye_count);

	spin_lock_irqsave(&fx->link->lock, flags);
	fx->link->needs_down = true;
	fx->link->down_reason = TBFRAME_DOWN_VERIFY;
	spin_unlock_irqrestore(&fx->link->lock, flags);
	tbframe_link_session_step(fx->link);

	KUNIT_EXPECT_EQ(test, 1u, fx->mock.bye_count);
	KUNIT_EXPECT_TRUE(test, fx->mock.bye_rings_started);
}

/*
 * Inbound BYE downs the session (reason LOGOUT) and the ack is withheld
 * until the session has actually left UP -- the ack certifies "no more
 * frames from this side", so acking from a live session would be the same
 * lie the READY_ACK gate closes.
 */
static void tbframe_teardown_inbound_bye_quiesces_then_acks(struct kunit *test)
{
	struct tbframe_mock_fixture *fx = test->priv;
	u8 msg[TBFRAME_WIRE_HELLO_MSG_SIZE];
	struct tbframe_wire_hello reply;
	struct tbframe_wire_info info;

	tbframe_mock_link_up(test, fx);
	fx->mock.have_response = false;

	KUNIT_ASSERT_GE(test,
			tbframe_mock_build_peer_msg(fx, TBFRAME_WIRE_OP_BYE,
						    msg, sizeof(msg)), 0);
	KUNIT_EXPECT_EQ(test, 1,
			tbframe_link_handle_packet(fx->link, msg, sizeof(msg)));
	/* Session still UP at dispatch time: no ack yet. */
	KUNIT_EXPECT_FALSE(test, fx->mock.have_response);

	flush_workqueue(fx->tf.wq);
	KUNIT_ASSERT_EQ(test, 1u, fx->client.down_count);
	KUNIT_EXPECT_EQ(test, (int)TBFRAME_DOWN_LOGOUT,
			fx->client.reasons[1]);

	/* The peer's BYE retry lands after the down: now it is acked. */
	KUNIT_EXPECT_EQ(test, 1,
			tbframe_link_handle_packet(fx->link, msg, sizeof(msg)));
	KUNIT_ASSERT_TRUE(test, fx->mock.have_response);
	KUNIT_ASSERT_EQ(test, 0,
			tbframe_wire_parse_hello(fx->mock.last_response,
						 sizeof(fx->mock.last_response),
						 &reply, &info));
	KUNIT_EXPECT_EQ(test, TBFRAME_WIRE_OP_BYE_ACK, info.op);

	/* LOGOUT re-handshakes automatically once the peer returns. */
	tbframe_link_session_step(fx->link);
	flush_workqueue(fx->tf.wq);
	KUNIT_EXPECT_EQ(test, 2u, fx->client.up_count);
}

static void tbframe_teardown_publisher_drain_poisons(struct kunit *test)
{
	struct tbframe_mock_fixture *fx = test->priv;
	struct tbframe_frame *f;
	unsigned long flags;

	tbframe_mock_link_up(test, fx);

	/* A publisher wedged inside the hardware never returns. */
	atomic_inc(&fx->link->hw_active);

	spin_lock_irqsave(&fx->link->lock, flags);
	fx->link->needs_down = true;
	fx->link->down_reason = TBFRAME_DOWN_VERIFY;
	spin_unlock_irqrestore(&fx->link->lock, flags);

	/* The bounded drain (xmit_drain_ms) expires and poisons the link. */
	tbframe_link_session_step(fx->link);

	KUNIT_ASSERT_EQ(test, 1u, fx->client.down_count);
	KUNIT_EXPECT_EQ(test, (int)TBFRAME_DOWN_DEAD_HW,
			fx->client.reasons[1]);
	KUNIT_EXPECT_EQ(test, (int)TBFRAME_STATE_DEAD, (int)fx->link->state);
	KUNIT_EXPECT_EQ(test, -ENETDOWN,
			tbframe_alloc_frame(fx->link, 64, false, &f));

	/* DEAD_HW is terminal: recovery never re-handshakes past it. */
	flush_workqueue(fx->tf.wq);
	tbframe_link_session_step(fx->link);
	flush_workqueue(fx->tf.wq);
	KUNIT_EXPECT_EQ(test, 1u, fx->client.up_count);
	KUNIT_EXPECT_EQ(test, (int)TBFRAME_STATE_DEAD, (int)fx->link->state);

	atomic_dec(&fx->link->hw_active);
}

static void tbframe_teardown_bounded_leak_no_hang(struct kunit *test)
{
	struct tbframe_mock_fixture *fx = test->priv;
	unsigned long start;
	int ret;

	tbframe_mock_link_up(test, fx);

	/*
	 * Dead hardware: stop_rings() cancels nothing, so every posted RX
	 * descriptor keeps its frame ref forever. Teardown must still
	 * complete: bounded wait (teardown_force_ms), then poison + leak.
	 */
	fx->mock.never_cancel = true;

	start = jiffies;
	ret = tbframe_link_destroy(fx->link, TBFRAME_DOWN_UNPLUG);
	fx->link_destroyed = true;

	/* No hang: returned in bounded time, well under the test timeout. */
	KUNIT_EXPECT_EQ(test, -EBUSY, ret);
	KUNIT_EXPECT_LT(test, jiffies_to_msecs(jiffies - start), 5000u);

	/* The leaked link is poisoned DEAD so nothing can re-arm it. */
	KUNIT_EXPECT_EQ(test, (int)TBFRAME_STATE_DEAD, (int)fx->link->state);
	KUNIT_EXPECT_GT(test, refcount_read(&fx->link->refcnt), 0u);

	/* The client saw its bracketing link_down. */
	KUNIT_ASSERT_EQ(test, 1u, fx->client.down_count);
	KUNIT_EXPECT_EQ(test, (int)TBFRAME_DOWN_UNPLUG,
			fx->client.reasons[1]);
}

static void tbframe_teardown_clean_when_hw_cancels(struct kunit *test)
{
	struct tbframe_mock_fixture *fx = test->priv;
	struct tbframe_frame *f;
	int ret;

	tbframe_mock_link_up(test, fx);

	/* A transmitted frame in flight is reclaimed by the ring fence. */
	KUNIT_ASSERT_EQ(test, 0, tbframe_alloc_frame(fx->link, 512, false, &f));
	KUNIT_ASSERT_EQ(test, 0, tbframe_xmit(fx->link, f));

	ret = tbframe_link_destroy(fx->link, TBFRAME_DOWN_UNPLUG);
	fx->link_destroyed = true;

	/* Healthy cancellation drains every ref: fully freed, no leak. */
	KUNIT_EXPECT_EQ(test, 0, ret);
	KUNIT_EXPECT_EQ(test, 1u, fx->client.down_count);
}

static int tbframe_teardown_test_init(struct kunit *test)
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

static void tbframe_teardown_test_exit(struct kunit *test)
{
	tbframe_mock_fixture_exit(test, test->priv);
}

static struct kunit_case tbframe_teardown_cases[] = {
	KUNIT_CASE(tbframe_teardown_stops_rings_before_paths),
	KUNIT_CASE(tbframe_teardown_sends_bye_while_rings_up),
	KUNIT_CASE(tbframe_teardown_inbound_bye_quiesces_then_acks),
	KUNIT_CASE(tbframe_teardown_publisher_drain_poisons),
	KUNIT_CASE(tbframe_teardown_bounded_leak_no_hang),
	KUNIT_CASE(tbframe_teardown_clean_when_hw_cancels),
	{}
};

static struct kunit_suite tbframe_teardown_suite = {
	.name = "tbframe_teardown",
	.init = tbframe_teardown_test_init,
	.exit = tbframe_teardown_test_exit,
	.test_cases = tbframe_teardown_cases,
};
kunit_test_suite(tbframe_teardown_suite);

MODULE_LICENSE("GPL");
