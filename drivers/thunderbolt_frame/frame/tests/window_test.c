// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit: Mode A flow-control arithmetic (spec §6).
 *
 * The data admission window is min(local cap, peer rx_ring_entries) minus
 * the control reserve; control frames are charged to the reserve so bulk
 * data can never starve ACKs; -ENOSPC closes the producer and a drained
 * completion reopens it through the tx_released() upcall.
 * Run via tools/run-kunit.sh.
 */
#include "tbframe_mock.h"

static void tbframe_window_arithmetic(struct kunit *test)
{
	struct tbframe_mock_fixture *fx = test->priv;
	struct tbframe_frame *f;
	struct tbframe_frame *last_data = NULL;
	u16 window = TBFRAME_MOCK_RING - TBFRAME_CTRL_RESERVE;
	u16 i;

	tbframe_mock_link_up(test, fx);
	KUNIT_ASSERT_EQ(test, window, fx->client.last_info.data_window);

	/* The data window admits exactly its size, then -ENOSPC. */
	for (i = 0; i < window; i++) {
		KUNIT_ASSERT_EQ(test, 0,
				tbframe_alloc_frame(fx->link, 1024, false, &f));
		last_data = f;
	}
	KUNIT_EXPECT_EQ(test, -ENOSPC,
			tbframe_alloc_frame(fx->link, 1024, false, &f));

	/* The control reserve is independent of the exhausted data window. */
	for (i = 0; i < TBFRAME_CTRL_RESERVE; i++)
		KUNIT_ASSERT_EQ(test, 0,
				tbframe_alloc_frame(fx->link, 64, true, &f));
	KUNIT_EXPECT_EQ(test, -ENOSPC,
			tbframe_alloc_frame(fx->link, 64, true, &f));

	/* Freeing an un-transmitted frame reopens its window. */
	tbframe_frame_free(fx->link, last_data);
	flush_workqueue(fx->tf.wq);
	KUNIT_EXPECT_GE(test, fx->client.tx_released_count, 1u);
	KUNIT_EXPECT_EQ(test, 0,
			tbframe_alloc_frame(fx->link, 1024, false, &f));
}

static void tbframe_window_tx_released_on_completion(struct kunit *test)
{
	struct tbframe_mock_fixture *fx = test->priv;
	struct tbframe_frame *f;
	u16 window = TBFRAME_MOCK_RING - TBFRAME_CTRL_RESERVE;
	u16 i;

	tbframe_mock_link_up(test, fx);

	/* Fill the window with transmitted frames. */
	for (i = 0; i < window; i++) {
		KUNIT_ASSERT_EQ(test, 0,
				tbframe_alloc_frame(fx->link, 512, false, &f));
		KUNIT_ASSERT_EQ(test, 0, tbframe_xmit(fx->link, f));
	}
	KUNIT_EXPECT_EQ(test, window, (u16)fx->mock.tx_queued);
	KUNIT_EXPECT_EQ(test, -ENOSPC,
			tbframe_alloc_frame(fx->link, 512, false, &f));
	KUNIT_EXPECT_EQ(test, 0u, fx->client.tx_released_count);

	/* One hardware completion reopens the window exactly once. */
	KUNIT_ASSERT_TRUE(test, tbframe_mock_complete_tx(fx));
	flush_workqueue(fx->tf.wq);
	KUNIT_EXPECT_EQ(test, 1u, fx->client.tx_released_count);
	KUNIT_EXPECT_EQ(test, 0,
			tbframe_alloc_frame(fx->link, 512, false, &f));
	tbframe_frame_free(fx->link, f);

	/* Unblocked completions do not spam tx_released. */
	KUNIT_ASSERT_TRUE(test, tbframe_mock_complete_tx(fx));
	flush_workqueue(fx->tf.wq);
	KUNIT_EXPECT_EQ(test, 1u, fx->client.tx_released_count);
}

static void tbframe_window_honors_peer_ring(struct kunit *test)
{
	struct tbframe_mock_fixture *fx = test->priv;

	/* A smaller peer RX ring, not the local cap, bounds the window. */
	fx->mock.peer.rx_ring_entries = 128;
	tbframe_mock_link_up(test, fx);
	KUNIT_EXPECT_EQ(test, (u16)(128 - TBFRAME_CTRL_RESERVE),
			fx->client.last_info.data_window);
}

static void tbframe_window_resets_on_link_down(struct kunit *test)
{
	struct tbframe_mock_fixture *fx = test->priv;
	struct tbframe_frame *f;
	u16 window = TBFRAME_MOCK_RING - TBFRAME_CTRL_RESERVE;
	u16 i;

	tbframe_mock_link_up(test, fx);
	for (i = 0; i < window; i++) {
		KUNIT_ASSERT_EQ(test, 0,
				tbframe_alloc_frame(fx->link, 512, false, &f));
		KUNIT_ASSERT_EQ(test, 0, tbframe_xmit(fx->link, f));
	}
	KUNIT_EXPECT_EQ(test, -ENOSPC,
			tbframe_alloc_frame(fx->link, 512, false, &f));

	/*
	 * Session down cancels every in-flight frame (stop_rings) and the
	 * admission window is whole again on the next establishment.
	 */
	fx->mock.paths_active_ret = 0;
	tbframe_link_verify_step(fx->link);
	flush_workqueue(fx->tf.wq);
	KUNIT_ASSERT_EQ(test, 1u, fx->client.down_count);

	fx->mock.paths_active_ret = 1;
	tbframe_link_session_step(fx->link);
	KUNIT_ASSERT_EQ(test, 2u, fx->client.up_count);
	for (i = 0; i < window; i++)
		KUNIT_ASSERT_EQ(test, 0,
				tbframe_alloc_frame(fx->link, 512, false, &f));
	KUNIT_EXPECT_EQ(test, -ENOSPC,
			tbframe_alloc_frame(fx->link, 512, false, &f));
}

/*
 * Bounded ring residency: with tx_ring_budget set, at most budget frames
 * sit in the hardware TX ring; the rest wait in a software queue. This is
 * what keeps ACK queueing delay at microseconds instead of a full ring
 * (2048 x 4KiB ~ 9ms at wire rate) -- the qps32 bidirectional rewind
 * storm was ACK arrival gaps exceeding the retransmit base purely from
 * ring FIFO depth.
 */
static void tbframe_txq_budget_bounds_ring(struct kunit *test)
{
	struct tbframe_mock_fixture *fx = test->priv;
	struct tbframe_frame *f;
	u16 i;

	fx->tf.tx_ring_budget = 4;
	tbframe_mock_link_up(test, fx);

	for (i = 0; i < 10; i++) {
		KUNIT_ASSERT_EQ(test, 0,
				tbframe_alloc_frame(fx->link, 512, false, &f));
		KUNIT_ASSERT_EQ(test, 0, tbframe_xmit(fx->link, f));
	}
	/* Only the budget reaches the hardware ring. */
	KUNIT_EXPECT_EQ(test, 4u, fx->mock.tx_queued);

	/* One-in-one-out: each completion admits exactly one queued frame. */
	KUNIT_ASSERT_TRUE(test, tbframe_mock_complete_tx(fx));
	KUNIT_EXPECT_EQ(test, 5u, fx->mock.tx_queued);

	/* Draining the ring drains the backlog completely. */
	while (tbframe_mock_complete_tx(fx))
		;
	KUNIT_EXPECT_EQ(test, 10u, fx->mock.tx_queued);
	flush_workqueue(fx->tf.wq);
}

/* A ctrl frame (rxe ACK, keepalive) overtakes the queued data backlog. */
static void tbframe_txq_ctrl_jumps_queue(struct kunit *test)
{
	struct tbframe_mock_fixture *fx = test->priv;
	struct tbframe_frame *f;
	struct tbframe_frame_priv *fp;
	struct ring_frame *rf;
	u16 i;

	fx->tf.tx_ring_budget = 4;
	tbframe_mock_link_up(test, fx);

	for (i = 0; i < 6; i++) {
		KUNIT_ASSERT_EQ(test, 0,
				tbframe_alloc_frame(fx->link, 512, false, &f));
		KUNIT_ASSERT_EQ(test, 0, tbframe_xmit(fx->link, f));
	}
	KUNIT_ASSERT_EQ(test, 0, tbframe_alloc_frame(fx->link, 64, true, &f));
	KUNIT_ASSERT_EQ(test, 0, tbframe_xmit(fx->link, f));
	KUNIT_EXPECT_EQ(test, 4u, fx->mock.tx_queued);

	/* The next refill posts the ctrl frame, ahead of 2 queued data. */
	KUNIT_ASSERT_TRUE(test, tbframe_mock_complete_tx(fx));
	KUNIT_ASSERT_EQ(test, 5u, fx->mock.tx_queued);
	rf = list_last_entry(&fx->mock.tx_queue, struct ring_frame, list);
	fp = container_of(rf, struct tbframe_frame_priv, rf);
	KUNIT_EXPECT_TRUE(test, fp->frame.is_ctrl);

	/* After the ctrl frame, data resumes in FIFO order. */
	KUNIT_ASSERT_TRUE(test, tbframe_mock_complete_tx(fx));
	rf = list_last_entry(&fx->mock.tx_queue, struct ring_frame, list);
	fp = container_of(rf, struct tbframe_frame_priv, rf);
	KUNIT_EXPECT_FALSE(test, fp->frame.is_ctrl);

	while (tbframe_mock_complete_tx(fx))
		;
	flush_workqueue(fx->tf.wq);
}

/* Session down returns the software backlog; nothing posts afterwards. */
static void tbframe_txq_flushes_on_down(struct kunit *test)
{
	struct tbframe_mock_fixture *fx = test->priv;
	struct tbframe_frame *f;
	u16 window = TBFRAME_MOCK_RING - TBFRAME_CTRL_RESERVE;
	u16 i;

	fx->tf.tx_ring_budget = 2;
	tbframe_mock_link_up(test, fx);

	for (i = 0; i < 6; i++) {
		KUNIT_ASSERT_EQ(test, 0,
				tbframe_alloc_frame(fx->link, 512, false, &f));
		KUNIT_ASSERT_EQ(test, 0, tbframe_xmit(fx->link, f));
	}
	KUNIT_EXPECT_EQ(test, 2u, fx->mock.tx_queued);

	fx->mock.paths_active_ret = 0;
	tbframe_link_verify_step(fx->link);
	flush_workqueue(fx->tf.wq);
	KUNIT_ASSERT_EQ(test, 1u, fx->client.down_count);
	/* The flush never posted the backlog into the dying ring. */
	KUNIT_EXPECT_EQ(test, 2u, fx->mock.tx_queued);

	/* Every frame (posted and queued) is back: the window is whole. */
	fx->mock.paths_active_ret = 1;
	tbframe_link_session_step(fx->link);
	KUNIT_ASSERT_EQ(test, 2u, fx->client.up_count);
	for (i = 0; i < window; i++)
		KUNIT_ASSERT_EQ(test, 0,
				tbframe_alloc_frame(fx->link, 512, false, &f));
	KUNIT_EXPECT_EQ(test, -ENOSPC,
			tbframe_alloc_frame(fx->link, 512, false, &f));
}

/* Diagnostics describe hardware residency, not software-queue admission. */
static void tbframe_txq_counters_follow_hardware(struct kunit *test)
{
	struct tbframe_mock_fixture *fx = test->priv;
	struct tbframe_frame *f;
	u64 submitted, done;
	u16 i;

	fx->tf.tx_ring_budget = 2;
	tbframe_mock_link_up(test, fx);
	submitted = fx->link->data_tx_submitted;
	done = fx->link->data_tx_done;

	for (i = 0; i < 4; i++) {
		KUNIT_ASSERT_EQ(test, 0,
				tbframe_alloc_frame(fx->link, 512, false, &f));
		KUNIT_ASSERT_EQ(test, 0, tbframe_xmit(fx->link, f));
	}
	KUNIT_EXPECT_EQ(test, submitted + 2, fx->link->data_tx_submitted);
	KUNIT_EXPECT_EQ(test, done, fx->link->data_tx_done);

	KUNIT_ASSERT_TRUE(test, tbframe_mock_complete_tx(fx));
	KUNIT_EXPECT_EQ(test, submitted + 3, fx->link->data_tx_submitted);
	KUNIT_EXPECT_EQ(test, done + 1, fx->link->data_tx_done);

	while (tbframe_mock_complete_tx(fx))
		;
	KUNIT_EXPECT_EQ(test, submitted + 4, fx->link->data_tx_submitted);
	KUNIT_EXPECT_EQ(test, done + 4, fx->link->data_tx_done);
}

/* Canceling a software backlog is not a hardware-ring cancellation. */
static void tbframe_txq_canceled_counts_only_posted(struct kunit *test)
{
	struct tbframe_mock_fixture *fx = test->priv;
	struct tbframe_frame *f;
	u64 canceled;
	u16 i;

	fx->tf.tx_ring_budget = 2;
	tbframe_mock_link_up(test, fx);
	canceled = fx->link->data_tx_canceled;

	for (i = 0; i < 4; i++) {
		KUNIT_ASSERT_EQ(test, 0,
				tbframe_alloc_frame(fx->link, 512, false, &f));
		KUNIT_ASSERT_EQ(test, 0, tbframe_xmit(fx->link, f));
	}

	fx->mock.paths_active_ret = 0;
	tbframe_link_verify_step(fx->link);
	flush_workqueue(fx->tf.wq);
	KUNIT_EXPECT_EQ(test, canceled + 2, fx->link->data_tx_canceled);
}

static int tbframe_window_test_init(struct kunit *test)
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

static void tbframe_window_test_exit(struct kunit *test)
{
	tbframe_mock_fixture_exit(test, test->priv);
}

static struct kunit_case tbframe_window_cases[] = {
	KUNIT_CASE(tbframe_window_arithmetic),
	KUNIT_CASE(tbframe_window_tx_released_on_completion),
	KUNIT_CASE(tbframe_window_honors_peer_ring),
	KUNIT_CASE(tbframe_window_resets_on_link_down),
	KUNIT_CASE(tbframe_txq_budget_bounds_ring),
	KUNIT_CASE(tbframe_txq_ctrl_jumps_queue),
	KUNIT_CASE(tbframe_txq_flushes_on_down),
	KUNIT_CASE(tbframe_txq_counters_follow_hardware),
	KUNIT_CASE(tbframe_txq_canceled_counts_only_posted),
	{}
};

static struct kunit_suite tbframe_window_suite = {
	.name = "tbframe_window",
	.init = tbframe_window_test_init,
	.exit = tbframe_window_test_exit,
	.test_cases = tbframe_window_cases,
};
kunit_test_suite(tbframe_window_suite);

MODULE_LICENSE("GPL");
