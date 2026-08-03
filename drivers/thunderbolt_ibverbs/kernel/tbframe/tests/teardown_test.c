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
