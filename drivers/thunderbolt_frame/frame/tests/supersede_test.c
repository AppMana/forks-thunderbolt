// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit: tbframe supersede and zombie contract
 * (thunderbolt_negotiation.h semantics).
 *
 * A fresh inbound HELLO while established means the peer restarted without
 * a link edge: the session must be superseded, torn down and re-negotiated.
 * A keepalive frame carrying an unexpected session cookie is the same
 * signal caught inside one verify interval. Run via tools/run-kunit.sh.
 */
#include "tbframe_mock.h"

static void tbframe_supersede_on_rehello(struct kunit *test)
{
	struct tbframe_mock_fixture *fx = test->priv;
	u8 msg[TBFRAME_WIRE_HELLO_MSG_SIZE];

	tbframe_mock_link_up(test, fx);

	/* Peer reboots and re-HELLOs with a fresh cookie and hop. */
	fx->mock.peer.session_cookie = 0x9999888877776666ull;
	fx->mock.peer.transmit_hopid = 11;
	KUNIT_ASSERT_GE(test,
			tbframe_mock_build_peer_msg(fx, TBFRAME_WIRE_OP_HELLO,
						    msg, sizeof(msg)), 0);
	KUNIT_EXPECT_EQ(test, 1,
			tbframe_link_handle_packet(fx->link, msg, sizeof(msg)));

	/* The old lifetime is fenced before a retry receives HELLO_ACK. */
	KUNIT_EXPECT_FALSE(test, fx->mock.have_response);

	/* Session work runs the supersede: down, then full re-handshake. */
	flush_workqueue(fx->tf.wq);
	tbframe_link_session_step(fx->link);
	flush_workqueue(fx->tf.wq);

	KUNIT_ASSERT_GE(test, fx->client.down_count, 1u);
	KUNIT_EXPECT_EQ(test, (int)TBFRAME_DOWN_SUPERSEDE,
			fx->client.reasons[1]);
	KUNIT_EXPECT_EQ(test, 2u, fx->client.up_count);
	/* The new session took the peer's new transmit HopID. */
	KUNIT_EXPECT_EQ(test, 11, fx->mock.in_hopid);
}

/*
 * A HELLO for a new peer lifetime is not merely a control-plane update.  The
 * old data rings, hop entries and HopID still name the previous lifetime until
 * session work has stopped the rings and removed those paths.  An early ACK
 * lets the peer install its replacement path against that stale hardware.
 * The peer already retries HELLO, so withhold the reply until the old hardware
 * lifetime has crossed the stop -> disable -> release fence.
 */
static void
tbframe_supersede_hello_ack_waits_for_old_hw_fence(struct kunit *test)
{
	struct tbframe_mock_fixture *fx = test->priv;
	u8 msg[TBFRAME_WIRE_HELLO_MSG_SIZE];
	int stop, disable, release;

	tbframe_mock_link_up(test, fx);
	fx->mock.have_response = false;

	/* Keep session work behind us while dispatch sees the new lifetime. */
	mutex_lock(&fx->link->session_lock);
	fx->mock.peer.session_cookie = 0x5a5a5a5a5a5a5a5aull;
	fx->mock.peer.transmit_hopid = 13;
	KUNIT_ASSERT_GE(test,
			tbframe_mock_build_peer_msg(fx, TBFRAME_WIRE_OP_HELLO,
						    msg, sizeof(msg)), 0);
	KUNIT_EXPECT_EQ(test, 1,
			tbframe_link_handle_packet(fx->link, msg, sizeof(msg)));

	/* No reply may certify replacement paths while the old paths remain. */
	KUNIT_EXPECT_FALSE(test, fx->mock.have_response);
	KUNIT_EXPECT_TRUE(test, fx->mock.rings_started);
	KUNIT_EXPECT_TRUE(test, fx->mock.paths_on);

	mutex_unlock(&fx->link->session_lock);
	flush_workqueue(fx->tf.wq);

	stop = tbframe_mock_hw_call_pos(&fx->mock, TBFRAME_HW_STOP_RINGS, 0);
	disable = tbframe_mock_hw_call_pos(&fx->mock,
					   TBFRAME_HW_DISABLE_PATHS, 0);
	release = tbframe_mock_hw_call_pos(&fx->mock,
					   TBFRAME_HW_RELEASE_IN_HOPID, 0);
	KUNIT_ASSERT_GE(test, stop, 0);
	KUNIT_ASSERT_GE(test, disable, 0);
	KUNIT_ASSERT_GE(test, release, 0);
	KUNIT_EXPECT_LT(test, stop, disable);
	KUNIT_EXPECT_LT(test, disable, release);
}

/*
 * READY certification vs a pending supersede teardown.
 *
 * The reload race: a peer whose client bounced re-HELLOs (queueing our
 * supersede down) and immediately follows with READY, all from the dispatch
 * context, before the session work has run the teardown. The old code read
 * ->paths_enabled -- still true for the OLD session's paths, which the queued
 * down_session() is about to rip out -- and certified mutual readiness with
 * READY_ACK. The peer then went UP and streamed a full TX ring into a hop
 * entry we were disabling; on the 023/025 canaries that wedged the peer's
 * egress HopID at the router level (TX ring enabled, 2047 posted, zero
 * consumed) until a reboot. The ack must be withheld while a down is
 * pending, exactly as it is withheld before the paths first come up; the
 * peer's READY retry budget absorbs the delay.
 */
static void tbframe_ready_ack_withheld_during_pending_supersede(struct kunit *test)
{
	struct tbframe_mock_fixture *fx = test->priv;
	u8 msg[TBFRAME_WIRE_HELLO_MSG_SIZE];
	struct tbframe_wire_hello reply;
	struct tbframe_wire_info info;

	tbframe_mock_link_up(test, fx);

	/*
	 * Hold the session lock: the HELLO handler queues the supersede
	 * teardown but the session work cannot run it yet, exactly the
	 * dispatch-outruns-worker window of a fast peer reload.
	 */
	mutex_lock(&fx->link->session_lock);

	fx->mock.peer.session_cookie = 0x5a5a5a5a5a5a5a5aull;
	fx->mock.peer.transmit_hopid = 13;
	KUNIT_ASSERT_GE(test,
			tbframe_mock_build_peer_msg(fx, TBFRAME_WIRE_OP_HELLO,
						    msg, sizeof(msg)), 0);
	KUNIT_EXPECT_EQ(test, 1,
			tbframe_link_handle_packet(fx->link, msg, sizeof(msg)));
	KUNIT_EXPECT_FALSE(test, fx->mock.have_response);
	fx->mock.have_response = false;

	/* Peer's READY lands while our down is still queued: refused
	 * outright (a mid-teardown session does not negotiate), so no
	 * READY_ACK can certify the stale paths the teardown is about to
	 * remove.
	 */
	KUNIT_ASSERT_GE(test,
			tbframe_mock_build_peer_msg(fx, TBFRAME_WIRE_OP_READY,
						    msg, sizeof(msg)), 0);
	KUNIT_EXPECT_EQ(test, 0,
			tbframe_link_handle_packet(fx->link, msg, sizeof(msg)));
	KUNIT_EXPECT_FALSE(test, fx->mock.have_response);

	mutex_unlock(&fx->link->session_lock);

	/* The supersede runs and the session re-establishes cleanly. */
	flush_workqueue(fx->tf.wq);
	tbframe_link_session_step(fx->link);
	flush_workqueue(fx->tf.wq);
	KUNIT_ASSERT_GE(test, fx->client.down_count, 1u);
	KUNIT_EXPECT_EQ(test, (int)TBFRAME_DOWN_SUPERSEDE,
			fx->client.reasons[1]);
	KUNIT_EXPECT_EQ(test, 2u, fx->client.up_count);

	/* With the new session's paths enabled, READY is acked again. */
	fx->mock.have_response = false;
	KUNIT_ASSERT_GE(test,
			tbframe_mock_build_peer_msg(fx, TBFRAME_WIRE_OP_READY,
						    msg, sizeof(msg)), 0);
	KUNIT_EXPECT_EQ(test, 1,
			tbframe_link_handle_packet(fx->link, msg, sizeof(msg)));
	KUNIT_ASSERT_TRUE(test, fx->mock.have_response);
	KUNIT_ASSERT_EQ(test, 0,
			tbframe_wire_parse_hello(fx->mock.last_response,
						 sizeof(fx->mock.last_response),
						 &reply, &info));
	KUNIT_EXPECT_EQ(test, TBFRAME_WIRE_OP_READY_ACK, info.op);
}

static void tbframe_cookie_mismatch_zombie(struct kunit *test)
{
	struct tbframe_mock_fixture *fx = test->priv;
	struct tbframe_frame_priv *f;

	tbframe_mock_link_up(test, fx);

	/* A keepalive echoing the negotiated cookie is benign. */
	f = tbframe_mock_pop_rx(fx);
	KUNIT_ASSERT_NOT_NULL(test, f);
	tbframe_wire_put_le64(f->frame.data, fx->mock.peer.session_cookie);
	tbframe_core_rx_complete(f, false, TBFRAME_KEEPALIVE_LEN,
				 TBFRAME_PDF_KEEPALIVE, false);
	flush_workqueue(fx->tf.wq);
	KUNIT_EXPECT_EQ(test, 0u, fx->client.down_count);
	/* Keepalives are tbframe-internal; the client never sees them. */
	KUNIT_EXPECT_EQ(test, 0u, fx->client.rx_count);

	/*
	 * A mismatched cookie means the peer rebooted inside one verify
	 * interval: supersede signal, full session reset, re-handshake.
	 */
	f = tbframe_mock_pop_rx(fx);
	KUNIT_ASSERT_NOT_NULL(test, f);
	tbframe_wire_put_le64(f->frame.data, ~fx->mock.peer.session_cookie);
	tbframe_core_rx_complete(f, false, TBFRAME_KEEPALIVE_LEN,
				 TBFRAME_PDF_KEEPALIVE, false);
	flush_workqueue(fx->tf.wq);

	KUNIT_ASSERT_GE(test, fx->client.down_count, 1u);
	KUNIT_EXPECT_EQ(test, (int)TBFRAME_DOWN_SUPERSEDE,
			fx->client.reasons[1]);
	/* The re-handshake is queued with a retry delay; drive it directly. */
	tbframe_link_session_step(fx->link);
	KUNIT_EXPECT_EQ(test, 2u, fx->client.up_count);
}

static void tbframe_verify_sends_keepalive(struct kunit *test)
{
	struct tbframe_mock_fixture *fx = test->priv;
	struct ring_frame *rf;
	struct tbframe_frame_priv *f;

	tbframe_mock_link_up(test, fx);

	/* Keep the keepalive in the ring so its contents can be inspected. */
	fx->mock.hold_keepalives = true;

	KUNIT_EXPECT_EQ(test, 0u, fx->mock.tx_queued);
	tbframe_link_verify_step(fx->link);
	KUNIT_ASSERT_EQ(test, 1u, fx->mock.tx_queued);

	rf = list_first_entry(&fx->mock.tx_queue, struct ring_frame, list);
	f = container_of(rf, struct tbframe_frame_priv, rf);
	KUNIT_EXPECT_EQ(test, TBFRAME_PDF_KEEPALIVE, f->frame.pdf);
	KUNIT_EXPECT_TRUE(test, f->frame.is_ctrl);
	KUNIT_EXPECT_EQ(test, fx->link->local_cookie,
			tbframe_wire_get_le64(f->frame.data));

	/* Healthy paths: the session stays up. */
	KUNIT_EXPECT_EQ(test, 0u, fx->client.down_count);
	tbframe_mock_complete_tx(fx);
}

static void tbframe_no_keepalive_capability_stays_private(struct kunit *test)
{
	struct tbframe_mock_fixture *fx = test->priv;

	/* Without capability bit 1 there is no session-bound data proof. */
	fx->mock.peer.capabilities = 0;
	tbframe_link_session_step(fx->link);
	KUNIT_EXPECT_EQ(test, 0u, fx->client.up_count);
	KUNIT_EXPECT_EQ(test, 0u, fx->mock.tx_queued);
}

static int tbframe_supersede_test_init(struct kunit *test)
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

static void tbframe_supersede_test_exit(struct kunit *test)
{
	tbframe_mock_fixture_exit(test, test->priv);
}

static struct kunit_case tbframe_supersede_cases[] = {
	KUNIT_CASE(tbframe_supersede_on_rehello),
	KUNIT_CASE(tbframe_supersede_hello_ack_waits_for_old_hw_fence),
	KUNIT_CASE(tbframe_ready_ack_withheld_during_pending_supersede),
	KUNIT_CASE(tbframe_cookie_mismatch_zombie),
	KUNIT_CASE(tbframe_verify_sends_keepalive),
	KUNIT_CASE(tbframe_no_keepalive_capability_stays_private),
	{}
};

static struct kunit_suite tbframe_supersede_suite = {
	.name = "tbframe_supersede",
	.init = tbframe_supersede_test_init,
	.exit = tbframe_supersede_test_exit,
	.test_cases = tbframe_supersede_cases,
};
kunit_test_suite(tbframe_supersede_suite);

MODULE_LICENSE("GPL");
