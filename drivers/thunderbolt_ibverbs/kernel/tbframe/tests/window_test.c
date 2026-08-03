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
