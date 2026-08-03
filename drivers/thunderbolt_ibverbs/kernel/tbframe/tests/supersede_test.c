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
	struct tbframe_wire_hello ack;
	struct tbframe_wire_info info;

	tbframe_mock_link_up(test, fx);

	/* Peer reboots and re-HELLOs with a fresh cookie and hop. */
	fx->mock.peer.session_cookie = 0x9999888877776666ull;
	fx->mock.peer.transmit_hopid = 11;
	KUNIT_ASSERT_GE(test,
			tbframe_mock_build_peer_msg(fx, TBFRAME_WIRE_OP_HELLO,
						    msg, sizeof(msg)), 0);
	KUNIT_EXPECT_EQ(test, 1,
			tbframe_link_handle_packet(fx->link, msg, sizeof(msg)));

	/* The handler acks the HELLO with our current session parameters. */
	KUNIT_ASSERT_TRUE(test, fx->mock.have_response);
	KUNIT_ASSERT_EQ(test, 0,
			tbframe_wire_parse_hello(fx->mock.last_response,
						 sizeof(fx->mock.last_response),
						 &ack, &info));
	KUNIT_EXPECT_EQ(test, TBFRAME_WIRE_OP_HELLO_ACK, info.op);
	KUNIT_EXPECT_EQ(test, (u16)TBFRAME_MOCK_RING, ack.rx_ring_entries);

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

static void tbframe_no_keepalive_without_capability(struct kunit *test)
{
	struct tbframe_mock_fixture *fx = test->priv;

	/* Peer did not advertise capability bit 1: never send keepalives. */
	fx->mock.peer.capabilities = 0;
	tbframe_mock_link_up(test, fx);
	tbframe_link_verify_step(fx->link);
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
	KUNIT_CASE(tbframe_cookie_mismatch_zombie),
	KUNIT_CASE(tbframe_verify_sends_keepalive),
	KUNIT_CASE(tbframe_no_keepalive_without_capability),
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
