// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit contract for the UNLOAD and SHUTDOWN teardown paths.
 *
 * Hard rule under test, from the appmana-019 (2026-08-24 live reload, hard
 * hang, physical power cycle) and appmana-008 (2026-08-24 `systemctl reboot`
 * that never came back) incidents: every wait on the far end must be bounded
 * with a defined failure action, and nothing may block indefinitely. A live
 * rmmod may REFUSE; a shutdown may not -- a shutdown must bound its waits and
 * proceed, because leaking a ring is strictly better than wedging a node that
 * an operator believed they were rebooting.
 *
 * The modelled configuration is appmana-008's: a peer that is actively
 * retrying HELLO into us while our own session is in the failed/retry state,
 * and a teardown running concurrently.
 */
#include <kunit/test.h>

#include "tbframe_mock.h"

static struct tbframe_mock_fixture *ul_fx(struct kunit *test)
{
	return test->priv;
}

static int ul_init(struct kunit *test)
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

static void ul_exit(struct kunit *test)
{
	tbframe_mock_fixture_exit(test, ul_fx(test));
}

static void ul_peer_hello(struct kunit *test, struct tbframe_mock_fixture *fx)
{
	u8 msg[TBFRAME_WIRE_HELLO_MSG_SIZE];

	KUNIT_ASSERT_GE(test,
			tbframe_mock_build_peer_msg(fx, TBFRAME_WIRE_OP_HELLO,
						    msg, sizeof(msg)), 0);
	tbframe_link_handle_packet(fx->link, msg, sizeof(msg));
}

/*
 * (a) Teardown racing inbound peer traffic into a ring being freed.
 *
 * A peer that keeps HELLOing through our unload must not be able to mutate the
 * session behind the teardown's back, and must not be able to get a frame
 * posted to a ring that has already been freed. The mock's post_rx refuses
 * once free_rings has run (-ESHUTDOWN), so a post after the free would be a
 * ring-use-after-free; assert none happened.
 */
static void tbframe_unload_peer_hello_during_teardown(struct kunit *test)
{
	struct tbframe_mock_fixture *fx = ul_fx(test);
	unsigned int posts_before;

	tbframe_mock_link_up(test, fx);

	/* The peer is HELLOing at us as the unload starts. */
	ul_peer_hello(test, fx);

	posts_before = fx->mock.rx_posted_count;
	KUNIT_ASSERT_GT(test, posts_before, 0u);

	KUNIT_EXPECT_EQ(test, 0, tbframe_link_destroy(fx->link,
						      TBFRAME_DOWN_CLOSED));
	fx->link_destroyed = true;

	/* Rings are gone and every descriptor came back through the pool. */
	KUNIT_EXPECT_FALSE(test, fx->mock.rings_alloced);
	KUNIT_EXPECT_EQ(test, 0u, fx->mock.rx_posted_count);
	/* Exactly one in-HopID allocated and exactly one released. */
	KUNIT_EXPECT_EQ(test, fx->mock.in_hopid_allocs,
			fx->mock.in_hopid_releases);
	KUNIT_EXPECT_EQ(test, fx->mock.last_alloc_in_hopid,
			fx->mock.last_release_in_hopid);
}

/*
 * A HELLO that lands after ->removing is set must be refused outright: an
 * accepted one would rewrite remote_hopid from the dispatch context and make
 * the teardown release a HopID it never allocated.
 */
static void tbframe_unload_hello_after_removing_is_refused(struct kunit *test)
{
	struct tbframe_mock_fixture *fx = ul_fx(test);
	u16 hopid_before;
	u8 msg[TBFRAME_WIRE_HELLO_MSG_SIZE];

	tbframe_mock_link_up(test, fx);
	hopid_before = fx->mock.last_alloc_in_hopid;

	/* Enter the teardown fence the way tbframe_link_destroy() does. */
	{
		unsigned long flags;

		spin_lock_irqsave(&fx->link->lock, flags);
		fx->link->removing = true;
		spin_unlock_irqrestore(&fx->link->lock, flags);
	}

	fx->mock.peer.transmit_hopid = hopid_before + 7;
	KUNIT_ASSERT_GE(test,
			tbframe_mock_build_peer_msg(fx, TBFRAME_WIRE_OP_HELLO,
						    msg, sizeof(msg)), 0);
	/* Not consumed, and no session state moved. */
	KUNIT_EXPECT_EQ(test, 0,
			tbframe_link_handle_packet(fx->link, msg, sizeof(msg)));

	KUNIT_EXPECT_EQ(test, 0, tbframe_link_destroy(fx->link,
						      TBFRAME_DOWN_CLOSED));
	fx->link_destroyed = true;
	KUNIT_EXPECT_EQ(test, (int)hopid_before, fx->mock.last_release_in_hopid);
}

/*
 * (c) A peer that never answers during teardown.
 *
 * The BYE quiesce must burn its budget and then PROCEED. It may not retry
 * forever and it may not skip the local teardown. Bounded-and-proceed.
 */
static void tbframe_unload_silent_peer_is_bounded(struct kunit *test)
{
	struct tbframe_mock_fixture *fx = ul_fx(test);

	tbframe_mock_link_up(test, fx);

	/* Peer goes away entirely: every control request times out. */
	fx->mock.control_err = -ETIMEDOUT;

	KUNIT_EXPECT_EQ(test, 0, tbframe_link_destroy(fx->link,
						      TBFRAME_DOWN_CLOSED));
	fx->link_destroyed = true;

	/*
	 * Exactly the budget, not one more, not zero. Counted from the request
	 * log rather than bye_count: a peer that never answers never reaches
	 * the mock's ack path, which is the whole point of the case.
	 */
	KUNIT_EXPECT_EQ(test, (unsigned int)TBFRAME_BYE_RETRIES,
			tbframe_mock_count_req_op(&fx->mock,
						  TBFRAME_WIRE_OP_BYE));
	/* ...and the teardown still completed in full. */
	KUNIT_EXPECT_FALSE(test, fx->mock.rings_alloced);
	KUNIT_EXPECT_FALSE(test, fx->mock.paths_on);
	KUNIT_EXPECT_EQ(test, fx->mock.in_hopid_allocs,
			fx->mock.in_hopid_releases);
}

/*
 * A never-cancelling ring (dead hardware that never runs a frame callback)
 * must not be able to wedge the unload. teardown_force_ms=0 used to mean
 * "wait forever", which is an unbounded wait by configuration; it must now
 * select the built-in ceiling and force-proceed with the documented leak,
 * returning -EBUSY so the caller knows to leak the hw context too.
 */
static void tbframe_unload_never_waits_forever(struct kunit *test)
{
	struct tbframe_mock_fixture *fx = ul_fx(test);

	tbframe_mock_link_up(test, fx);

	fx->mock.never_cancel = true;
	/* The configuration that used to mean "no cap". */
	fx->tf.teardown_warn_ms = 20;
	fx->tf.teardown_force_ms = 0;
	/* ...and shutdown mode clamps it further still. */
	fx->tf.shutdown_mode = true;

	KUNIT_EXPECT_EQ(test, -EBUSY,
			tbframe_link_destroy(fx->link, TBFRAME_DOWN_CLOSED));
	fx->link_destroyed = true;
}

/*
 * (b-companion) Shutdown teardown, appmana-008's configuration: leaf reload
 * has left the session in the failed/retry state, the peer is HELLOing at us
 * endlessly, and the machine is rebooting.
 *
 * Refusing is not available here. The quiesce must run to completion, must
 * NOT spend the BYE budget on a peer during a fleet-wide reboot, and must
 * leave the hardware down so nhi_shutdown() does not find live rings.
 */
static void tbframe_unload_shutdown_quiesce_is_silent_and_complete(struct kunit *test)
{
	struct tbframe_mock_fixture *fx = ul_fx(test);

	tbframe_mock_link_up(test, fx);

	fx->tf.shutdown_mode = true;
	fx->mock.control_err = -ETIMEDOUT;	/* peer will not answer */
	ul_peer_hello(test, fx);		/* ...but keeps HELLOing */

	tbframe_link_shutdown(fx->link);

	/* No BYE at all: a shutdown does not wait on a peer. */
	KUNIT_EXPECT_EQ(test, 0u,
			tbframe_mock_count_req_op(&fx->mock,
						  TBFRAME_WIRE_OP_BYE));
	/* Hardware is fully down before nhi_shutdown() would run. */
	KUNIT_EXPECT_FALSE(test, fx->mock.rings_alloced);
	KUNIT_EXPECT_FALSE(test, fx->mock.rings_started);
	KUNIT_EXPECT_FALSE(test, fx->mock.paths_on);
	KUNIT_EXPECT_EQ(test, fx->mock.in_hopid_allocs,
			fx->mock.in_hopid_releases);
	/* The client saw its bracketing link_down. */
	KUNIT_EXPECT_EQ(test, 1u, fx->client.down_count);
}

/*
 * After the shutdown quiesce, a peer that is still HELLOing must not be able
 * to bring the session -- and the rings -- back up underneath the reboot.
 * That re-arm is the tbnet-side shape of the appmana-008 hang.
 */
static void tbframe_unload_shutdown_cannot_be_rearmed(struct kunit *test)
{
	struct tbframe_mock_fixture *fx = ul_fx(test);
	unsigned int i;

	tbframe_mock_link_up(test, fx);
	fx->tf.shutdown_mode = true;
	tbframe_link_shutdown(fx->link);

	fx->mock.control_err = 0;		/* peer is alive and eager */
	for (i = 0; i < 4; i++) {
		ul_peer_hello(test, fx);
		tbframe_link_session_step(fx->link);
	}

	KUNIT_EXPECT_FALSE(test, fx->mock.rings_alloced);
	KUNIT_EXPECT_FALSE(test, fx->mock.paths_on);
	KUNIT_EXPECT_EQ(test, 1u, fx->client.up_count);	/* never a second up */
}

static void tbframe_unload_shutdown_quiets_all_links_before_first_path_disable(struct kunit *test)
{
	struct tbframe_mock_fixture *fx = ul_fx(test);
	struct tbframe_mock peer = { };
	struct tbframe_link *second;

	INIT_LIST_HEAD(&peer.tx_queue);
	INIT_LIST_HEAD(&peer.rx_posted);
	peer.in_hopid = -1;
	peer.last_alloc_in_hopid = -1;
	peer.last_release_in_hopid = -1;
	peer.last_disable_remote_hopid = -1;
	peer.route = TBFRAME_MOCK_ROUTE + 1;
	peer.paths_active_ret = 1;
	peer.peer = fx->mock.peer;
	peer.peer.transmit_hopid++;

	second = tbframe_link_create(&fx->tf, &tbframe_mock_ops, &peer,
				     peer.route,
				     TBFRAME_MOCK_LOCAL_EUI64 + 1, false);
	KUNIT_ASSERT_FALSE(test, IS_ERR(second));

	tbframe_mock_link_up(test, fx);
	tbframe_link_session_step(second);
	KUNIT_ASSERT_EQ(test, 2u, fx->client.up_count);

	fx->mock.must_be_quiet_before_disable = second;
	queue_delayed_work(fx->tf.wq, &second->session_work,
			   msecs_to_jiffies(60000));
	fx->tf.shutdown_mode = true;
	tbframe_prepare_shutdown(&fx->tf);
	tbframe_link_shutdown(fx->link);

	KUNIT_EXPECT_FALSE(test,
			   fx->mock.disable_started_before_all_links_parked);
	KUNIT_EXPECT_FALSE(test,
			   fx->mock.disable_started_before_all_activity_stopped);

	tbframe_link_shutdown(second);
	KUNIT_EXPECT_EQ(test, 0,
			tbframe_link_destroy(second, TBFRAME_DOWN_CLOSED));
}

/*
 * Client detach is a global state transition. Every link must stop its
 * session worker before the first link begins a wire-level teardown; otherwise
 * a second link can issue a path/config request while the first waits for BYE,
 * coupling two independent control transactions during module exit.
 */
static void tbframe_unload_parks_all_links_before_first_bye(struct kunit *test)
{
	struct tbframe_mock_fixture *fx = ul_fx(test);
	struct tbframe_mock peer = { };
	struct tbframe_link *second;

	INIT_LIST_HEAD(&peer.tx_queue);
	INIT_LIST_HEAD(&peer.rx_posted);
	peer.in_hopid = -1;
	peer.last_alloc_in_hopid = -1;
	peer.last_release_in_hopid = -1;
	peer.last_disable_remote_hopid = -1;
	peer.route = TBFRAME_MOCK_ROUTE + 1;
	peer.paths_active_ret = 1;
	peer.peer = fx->mock.peer;
	peer.peer.transmit_hopid++;

	second = tbframe_link_create(&fx->tf, &tbframe_mock_ops, &peer,
				     peer.route,
				     TBFRAME_MOCK_LOCAL_EUI64 + 1, false);
	KUNIT_ASSERT_FALSE(test, IS_ERR(second));

	tbframe_mock_link_up(test, fx);
	tbframe_link_session_step(second);
	KUNIT_ASSERT_EQ(test, 2u, fx->client.up_count);

	fx->mock.must_be_parked_before_bye = second;
	queue_delayed_work(fx->tf.wq, &second->session_work,
			   msecs_to_jiffies(60000));
	tbframe_unregister_client_tf(&fx->tf);

	KUNIT_EXPECT_FALSE(test,
			   fx->mock.bye_started_before_all_links_parked);
	KUNIT_EXPECT_FALSE(test,
			   fx->mock.bye_started_before_all_session_work_stopped);

	KUNIT_EXPECT_EQ(test, 0,
			tbframe_link_destroy(second, TBFRAME_DOWN_CLOSED));
}

static void tbframe_unload_service_stop_quiets_all_links_before_remove(struct kunit *test)
{
	struct tbframe_mock_fixture *fx = ul_fx(test);
	struct tbframe_mock peer = { };
	struct tbframe_link *second;

	INIT_LIST_HEAD(&peer.tx_queue);
	INIT_LIST_HEAD(&peer.rx_posted);
	peer.in_hopid = -1;
	peer.last_alloc_in_hopid = -1;
	peer.last_release_in_hopid = -1;
	peer.last_disable_remote_hopid = -1;
	peer.route = TBFRAME_MOCK_ROUTE + 1;
	peer.paths_active_ret = 1;
	peer.peer = fx->mock.peer;
	peer.peer.transmit_hopid++;

	second = tbframe_link_create(&fx->tf, &tbframe_mock_ops, &peer,
				     peer.route,
				     TBFRAME_MOCK_LOCAL_EUI64 + 1, false);
	KUNIT_ASSERT_FALSE(test, IS_ERR(second));

	tbframe_mock_link_up(test, fx);
	tbframe_link_session_step(second);
	KUNIT_ASSERT_EQ(test, 2u, fx->client.up_count);

	fx->mock.must_be_parked_before_bye = second;
	queue_delayed_work(fx->tf.wq, &second->session_work,
			   msecs_to_jiffies(60000));
	tbframe_prepare_stop(&fx->tf);

	KUNIT_EXPECT_EQ(test, 0,
			tbframe_link_destroy(fx->link, TBFRAME_DOWN_CLOSED));
	fx->link_destroyed = true;
	KUNIT_EXPECT_FALSE(test,
			   fx->mock.bye_started_before_all_links_parked);
	KUNIT_EXPECT_FALSE(test,
			   fx->mock.bye_started_before_all_session_work_stopped);

	KUNIT_EXPECT_EQ(test, 0,
			tbframe_link_destroy(second, TBFRAME_DOWN_CLOSED));
}

static struct kunit_case tbframe_unload_cases[] = {
	KUNIT_CASE(tbframe_unload_peer_hello_during_teardown),
	KUNIT_CASE(tbframe_unload_hello_after_removing_is_refused),
	KUNIT_CASE(tbframe_unload_silent_peer_is_bounded),
	KUNIT_CASE(tbframe_unload_never_waits_forever),
	KUNIT_CASE(tbframe_unload_shutdown_quiesce_is_silent_and_complete),
	KUNIT_CASE(tbframe_unload_shutdown_cannot_be_rearmed),
	KUNIT_CASE(tbframe_unload_shutdown_quiets_all_links_before_first_path_disable),
	KUNIT_CASE(tbframe_unload_parks_all_links_before_first_bye),
	KUNIT_CASE(tbframe_unload_service_stop_quiets_all_links_before_remove),
	{}
};

static struct kunit_suite tbframe_unload_suite = {
	.name = "tbframe_unload",
	.init = ul_init,
	.exit = ul_exit,
	.test_cases = tbframe_unload_cases,
};
kunit_test_suite(tbframe_unload_suite);
