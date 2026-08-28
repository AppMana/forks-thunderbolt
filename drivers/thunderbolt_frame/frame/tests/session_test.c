// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit: tbframe session establishment contract.
 *
 * HELLO/READY happy path against the mock ring layer, the
 * tbnet_connected_work bring-up ordering (every RX descriptor primed
 * before enable_paths), READY-ack withholding until paths are enabled,
 * and the link-event ordering guarantees (no xmit accepted before
 * link_up, no rx delivered after link_down).
 * Run via tools/run-kunit.sh.
 */
#include "tbframe_mock.h"

static void tbframe_session_happy_path(struct kunit *test)
{
	struct tbframe_mock_fixture *fx = test->priv;
	struct tbframe_link_info info;

	KUNIT_ASSERT_EQ(test, 0u, fx->client.up_count);

	tbframe_link_session_step(fx->link);

	/* One pass with a healthy peer: HELLO, bring-up, READY, link_up. */
	KUNIT_EXPECT_EQ(test, 1u, fx->client.up_count);
	KUNIT_ASSERT_GE(test, fx->mock.req_count, 2u);
	KUNIT_EXPECT_EQ(test, TBFRAME_WIRE_OP_HELLO, fx->mock.req_ops[0]);
	KUNIT_EXPECT_EQ(test, TBFRAME_WIRE_OP_READY, fx->mock.req_ops[1]);

	/* Bring-up ordering: rings up, RX fully primed before enable_paths. */
	KUNIT_EXPECT_TRUE(test, fx->mock.rings_started);
	KUNIT_EXPECT_TRUE(test, fx->mock.paths_on);
	KUNIT_EXPECT_EQ(test, (unsigned int)TBFRAME_MOCK_RING,
			fx->mock.rx_posted_before_enable);
	/* RX HopID equals the peer's advertised transmit HopID. */
	KUNIT_EXPECT_EQ(test, (int)fx->mock.peer.transmit_hopid,
			fx->mock.in_hopid);

	/* link_up carried the negotiated session attributes. */
	KUNIT_EXPECT_EQ(test, fx->mock.peer.gid_eui64,
			fx->client.last_info.gid_eui64);
	/* ... including the identity WE advertised in our own HELLO, so the
	 * client can publish the same GID the peer derives for us.
	 */
	KUNIT_EXPECT_EQ(test, (u64)TBFRAME_MOCK_LOCAL_EUI64,
			fx->client.last_info.local_gid_eui64);
	KUNIT_EXPECT_EQ(test, (u16)TBFRAME_MOCK_RING,
			fx->client.last_info.rx_ring_entries);
	KUNIT_EXPECT_EQ(test, (u16)(TBFRAME_MOCK_RING - TBFRAME_CTRL_RESERVE),
			fx->client.last_info.data_window);
	KUNIT_EXPECT_EQ(test, (u16)TBFRAME_MAX_FRAME,
			fx->client.last_info.max_payload);
	KUNIT_EXPECT_FALSE(test, fx->client.last_info.e2e);
	KUNIT_EXPECT_EQ(test, 2, fx->client.last_info.width);
	KUNIT_EXPECT_EQ(test, 20, fx->client.last_info.speed);

	tbframe_link_info(fx->link, &info);
	KUNIT_EXPECT_EQ(test, fx->client.last_info.data_window,
			info.data_window);
	KUNIT_EXPECT_STREQ(test, "tbframe0x301",
			   tbframe_link_name(fx->link));
}

static void tbframe_session_ready_withheld_until_paths(struct kunit *test)
{
	struct tbframe_mock_fixture *fx = test->priv;
	u8 msg[TBFRAME_WIRE_HELLO_MSG_SIZE];

	/* Inbound READY before our paths are enabled: recorded, not acked. */
	KUNIT_ASSERT_GE(test,
			tbframe_mock_build_peer_msg(fx, TBFRAME_WIRE_OP_READY,
						    msg, sizeof(msg)), 0);
	KUNIT_EXPECT_EQ(test, 1,
			tbframe_link_handle_packet(fx->link, msg, sizeof(msg)));
	KUNIT_EXPECT_FALSE(test, fx->mock.have_response);
	KUNIT_EXPECT_TRUE(test, fx->link->hs.peer_seen);
	KUNIT_EXPECT_EQ(test, 0u, fx->client.up_count);

	/* Our own READY times out; HELLO and bring-up still succeed. */
	fx->mock.fail_ready = true;
	tbframe_link_session_step(fx->link);
	KUNIT_EXPECT_TRUE(test, fx->mock.paths_on);
	KUNIT_EXPECT_EQ(test, 0u, fx->client.up_count);

	/* Re-delivered READY with paths enabled is acked and completes. */
	fx->mock.have_response = false;
	KUNIT_EXPECT_EQ(test, 1,
			tbframe_link_handle_packet(fx->link, msg, sizeof(msg)));
	KUNIT_EXPECT_TRUE(test, fx->mock.have_response);

	fx->mock.fail_ready = false;
	tbframe_link_session_step(fx->link);
	KUNIT_EXPECT_EQ(test, 1u, fx->client.up_count);
}

/*
 * An early inbound READY is a responder-side operation with its own lifetime.
 * Enabling the paths must finish that pending response; requiring the peer to
 * retransmit makes two otherwise healthy simultaneous handshakes incur a full
 * request timeout before either can converge.
 */
static void tbframe_session_early_ready_is_acked_after_paths(struct kunit *test)
{
	struct tbframe_mock_fixture *fx = test->priv;
	u8 msg[TBFRAME_WIRE_HELLO_MSG_SIZE];

	KUNIT_ASSERT_GE(test,
			tbframe_mock_build_peer_msg(fx, TBFRAME_WIRE_OP_READY,
						    msg, sizeof(msg)), 0);
	KUNIT_ASSERT_EQ(test, 1,
			tbframe_link_handle_packet(fx->link, msg, sizeof(msg)));
	KUNIT_ASSERT_FALSE(test, fx->mock.have_response);

	/* Keep the requester half red; only the deferred responder may reply. */
	fx->mock.fail_ready = true;
	tbframe_link_session_step(fx->link);

	KUNIT_EXPECT_TRUE(test, fx->mock.paths_on);
	KUNIT_EXPECT_TRUE(test, fx->mock.have_response);
}

static void tbframe_session_hello_retries_reannounce(struct kunit *test)
{
	struct tbframe_mock_fixture *fx = test->priv;
	int i;

	/* Unanswered HELLOs never give up; they re-announce and retry. */
	fx->mock.control_err = -ETIMEDOUT;
	for (i = 0; i < TBFRAME_HELLO_RETRIES; i++)
		tbframe_link_session_step(fx->link);
	KUNIT_EXPECT_EQ(test, 1u, fx->mock.reannounce_calls);
	KUNIT_EXPECT_EQ(test, 0u, fx->client.up_count);

	fx->mock.control_err = 0;
	tbframe_link_session_step(fx->link);
	KUNIT_EXPECT_EQ(test, 1u, fx->client.up_count);
}

static void tbframe_session_wrong_ack_cannot_advance_state(struct kunit *test)
{
	struct tbframe_mock_fixture *fx = test->priv;

	/* A late ACK for another operation must not complete HELLO. */
	fx->mock.response_op = TBFRAME_WIRE_OP_READY_ACK;
	tbframe_link_session_step(fx->link);
	KUNIT_EXPECT_FALSE(test, fx->link->hello_done);
	KUNIT_EXPECT_FALSE(test, fx->mock.rings_alloced);
	KUNIT_EXPECT_EQ(test, 0u, fx->client.up_count);

	/* The valid response then advances the ordinary session path. */
	fx->mock.response_op = 0;
	tbframe_link_session_step(fx->link);
	KUNIT_EXPECT_TRUE(test, fx->link->hello_done);
	KUNIT_EXPECT_EQ(test, 1u, fx->client.up_count);
}

/*
 * Reply packets belong to the requester state machine.  A delayed reply from
 * an earlier peer instance may still traverse the shared dispatch path before
 * request matching; it must not seed a new session with stale peer fields.
 */
static void tbframe_session_unsolicited_hello_ack_cannot_seed_session(struct kunit *test)
{
	struct tbframe_mock_fixture *fx = test->priv;
	u64 current_cookie = fx->mock.peer.session_cookie;
	u8 msg[TBFRAME_WIRE_HELLO_MSG_SIZE];

	fx->mock.peer.session_cookie ^= BIT_ULL(7);
	KUNIT_ASSERT_GE(test,
			tbframe_mock_build_peer_msg(fx,
						    TBFRAME_WIRE_OP_HELLO_ACK,
						    msg, sizeof(msg)),
			0);
	KUNIT_EXPECT_EQ(test, 0,
			tbframe_link_handle_packet(fx->link, msg, sizeof(msg)));

	/* Only a correlated control_request() response may finish HELLO. */
	KUNIT_EXPECT_FALSE(test, fx->link->hello_done);
	KUNIT_EXPECT_EQ(test, 0ull, fx->link->remote_cookie);

	fx->mock.peer.session_cookie = current_cookie;
	tbframe_link_session_step(fx->link);

	KUNIT_EXPECT_EQ(test, current_cookie, fx->link->remote_cookie);
	KUNIT_EXPECT_EQ(test, 1u, fx->client.up_count);
}

/*
 * READY belongs to the peer instance negotiated by HELLO.  A delayed READY
 * from an earlier instance must not satisfy the responder half of the current
 * handshake or receive an acknowledgement for paths it does not own.
 */
static void tbframe_session_stale_ready_cannot_complete_responder(struct kunit *test)
{
	struct tbframe_mock_fixture *fx = test->priv;
	u64 negotiated_cookie = fx->mock.peer.session_cookie;
	u8 msg[TBFRAME_WIRE_HELLO_MSG_SIZE];

	/* Negotiate HELLO and program paths without completing READY. */
	fx->mock.fail_ready = true;
	tbframe_link_session_step(fx->link);
	KUNIT_ASSERT_TRUE(test, fx->link->hello_done);
	KUNIT_ASSERT_TRUE(test, fx->mock.paths_on);
	KUNIT_ASSERT_FALSE(test, fx->link->hs.peer_seen);

	fx->mock.peer.session_cookie ^= BIT_ULL(17);
	KUNIT_ASSERT_NE(test, negotiated_cookie,
			fx->mock.peer.session_cookie);
	KUNIT_ASSERT_GE(test,
			tbframe_mock_build_peer_msg(fx, TBFRAME_WIRE_OP_READY,
						    msg, sizeof(msg)), 0);
	KUNIT_EXPECT_EQ(test, 1,
			tbframe_link_handle_packet(fx->link, msg, sizeof(msg)));

	KUNIT_EXPECT_FALSE(test, fx->link->hs.peer_seen);
	KUNIT_EXPECT_FALSE(test, fx->mock.have_response);
}

/*
 * The requester half has the same binding requirement: a delayed READY_ACK
 * from a prior peer instance cannot certify the paths negotiated by HELLO.
 */
static void tbframe_session_stale_ready_ack_cannot_complete_requester(struct kunit *test)
{
	struct tbframe_mock_fixture *fx = test->priv;
	u64 negotiated_cookie = fx->mock.peer.session_cookie;

	/* Leave the requester waiting after a valid HELLO and path setup. */
	fx->mock.fail_ready = true;
	tbframe_link_session_step(fx->link);
	KUNIT_ASSERT_TRUE(test, fx->link->hello_done);
	KUNIT_ASSERT_TRUE(test, fx->mock.paths_on);
	KUNIT_ASSERT_FALSE(test, fx->link->hs.request_sent);

	fx->mock.peer.session_cookie ^= BIT_ULL(23);
	KUNIT_ASSERT_NE(test, negotiated_cookie,
			fx->mock.peer.session_cookie);
	fx->mock.fail_ready = false;
	tbframe_link_session_step(fx->link);

	KUNIT_EXPECT_FALSE(test, fx->link->hs.request_sent);
	KUNIT_EXPECT_FALSE(test, fx->link->hs.peer_seen);
	KUNIT_EXPECT_EQ(test, 0u, fx->client.up_count);
}

/*
 * The inbound responder and outbound requester can observe control packets in
 * either order.  They must converge on one peer instance before either is
 * allowed to replace the session identity used by the data path.
 */
static void tbframe_session_crossed_hello_cookies_cannot_mix(struct kunit *test)
{
	struct tbframe_mock_fixture *fx = test->priv;
	u64 negotiated_cookie = fx->mock.peer.session_cookie;
	u8 msg[TBFRAME_WIRE_HELLO_MSG_SIZE];

	fx->mock.fail_ready = true;
	tbframe_link_session_step(fx->link);
	KUNIT_ASSERT_TRUE(test, fx->link->hello_done);
	KUNIT_ASSERT_EQ(test, negotiated_cookie, fx->link->remote_cookie);

	fx->mock.peer.session_cookie ^= BIT_ULL(31);
	KUNIT_ASSERT_GE(test,
			tbframe_mock_build_peer_msg(fx, TBFRAME_WIRE_OP_HELLO,
						    msg, sizeof(msg)), 0);
	mutex_lock(&fx->link->session_lock);
	KUNIT_EXPECT_EQ(test, 1,
			tbframe_link_handle_packet(fx->link, msg, sizeof(msg)));

	KUNIT_EXPECT_EQ(test, negotiated_cookie, fx->link->remote_cookie);
	KUNIT_EXPECT_FALSE(test, fx->link->hs.peer_seen);
	mutex_unlock(&fx->link->session_lock);
}

/*
 * A session cookie authenticates data frames for one negotiated lifetime.
 * Reusing it after teardown lets delayed frames from the old rings prove the
 * replacement session even though they predate its HELLO/READY exchange.
 */
static void tbframe_session_teardown_rotates_local_cookie(struct kunit *test)
{
	struct tbframe_mock_fixture *fx = test->priv;
	u64 old_cookie;

	tbframe_mock_link_up(test, fx);
	old_cookie = fx->link->local_cookie;

	fx->mock.paths_active_ret = 0;
	tbframe_link_verify_step(fx->link);
	flush_workqueue(fx->tf.wq);

	KUNIT_EXPECT_NE(test, old_cookie, fx->link->local_cookie);
}

static void tbframe_session_event_ordering(struct kunit *test)
{
	struct tbframe_mock_fixture *fx = test->priv;
	struct tbframe_frame_priv *f;
	struct tbframe_frame *txf;
	unsigned int i;

	/* No xmit accepted before link_up. */
	KUNIT_EXPECT_EQ(test, -ENETDOWN,
			tbframe_alloc_frame(fx->link, 64, false, &txf));

	tbframe_mock_link_up(test, fx);

	/* An RX frame delivered while UP reaches the client. */
	f = tbframe_mock_pop_rx(fx);
	KUNIT_ASSERT_NOT_NULL(test, f);
	tbframe_core_rx_complete(f, false, 128, TBFRAME_PDF_DATA, false);
	KUNIT_EXPECT_EQ(test, 1u, fx->client.rx_count);
	KUNIT_EXPECT_EQ(test, 128, fx->client.last_rx_len);

	/* Hold one descriptor back so the down-cancel cannot collect it. */
	f = tbframe_mock_pop_rx(fx);
	KUNIT_ASSERT_NOT_NULL(test, f);

	/* Level-triggered verify sees dead paths: down + re-handshake. */
	fx->mock.paths_active_ret = 0;
	tbframe_link_verify_step(fx->link);
	flush_workqueue(fx->tf.wq);
	KUNIT_ASSERT_GE(test, fx->client.down_count, 1u);
	KUNIT_EXPECT_EQ(test, (int)TBFRAME_DOWN_VERIFY,
			fx->client.reasons[2]);

	/* No rx after link_down: the held frame completes but is dropped. */
	tbframe_core_rx_complete(f, false, 256, TBFRAME_PDF_DATA, false);
	KUNIT_EXPECT_EQ(test, 1u, fx->client.rx_count);

	/* No xmit accepted until the session re-establishes. */
	KUNIT_EXPECT_EQ(test, -ENETDOWN,
			tbframe_alloc_frame(fx->link, 64, false, &txf));

	/* Recorded order: UP, RX, DOWN with nothing out of bracket. */
	KUNIT_ASSERT_GE(test, fx->client.count, 3u);
	KUNIT_EXPECT_EQ(test, (u8)TBFRAME_EV_UP, fx->client.events[0]);
	KUNIT_EXPECT_EQ(test, (u8)TBFRAME_EV_RX, fx->client.events[1]);
	KUNIT_EXPECT_EQ(test, (u8)TBFRAME_EV_DOWN, fx->client.events[2]);
	for (i = 3; i < fx->client.count; i++)
		KUNIT_EXPECT_NE(test, (u8)TBFRAME_EV_RX,
				fx->client.events[i]);

	/* The automatic re-handshake brings the link back up. */
	fx->mock.paths_active_ret = 1;
	flush_workqueue(fx->tf.wq);
	tbframe_link_session_step(fx->link);
	KUNIT_EXPECT_EQ(test, 2u, fx->client.up_count);
}

static int tbframe_session_test_init(struct kunit *test)
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

static void tbframe_session_test_exit(struct kunit *test)
{
	tbframe_mock_fixture_exit(test, test->priv);
}

static struct kunit_case tbframe_session_cases[] = {
	KUNIT_CASE(tbframe_session_happy_path),
	KUNIT_CASE(tbframe_session_ready_withheld_until_paths),
	KUNIT_CASE(tbframe_session_early_ready_is_acked_after_paths),
	KUNIT_CASE(tbframe_session_hello_retries_reannounce),
	KUNIT_CASE(tbframe_session_wrong_ack_cannot_advance_state),
	KUNIT_CASE(tbframe_session_unsolicited_hello_ack_cannot_seed_session),
	KUNIT_CASE(tbframe_session_stale_ready_cannot_complete_responder),
	KUNIT_CASE(tbframe_session_stale_ready_ack_cannot_complete_requester),
	KUNIT_CASE(tbframe_session_crossed_hello_cookies_cannot_mix),
	KUNIT_CASE(tbframe_session_teardown_rotates_local_cookie),
	KUNIT_CASE(tbframe_session_event_ordering),
	{}
};

static struct kunit_suite tbframe_session_suite = {
	.name = "tbframe_session",
	.init = tbframe_session_test_init,
	.exit = tbframe_session_test_exit,
	.test_cases = tbframe_session_cases,
};
kunit_test_suite(tbframe_session_suite);

MODULE_LICENSE("GPL");
