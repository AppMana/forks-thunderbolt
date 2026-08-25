// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit contract for the data-path proof (tbframe defect: "live reload leaves
 * the BULK DATAPATH DEAD while every health signal says healthy").
 *
 * Field shape being modelled, appmana chain, 2026-08-23 live v2.43 migration:
 * 5 of 10 links came back with links up, tbr-<peer> netdevs present,
 * usb4_rdma* HCAs published, ports PORT_ACTIVE, GIDs populated, module
 * versions correct, and dmesg showing "READY confirmed" / "link up" -- while
 * ib_send_bw between neighbours connected the QPs, exchanged GIDs and then
 * completed ZERO iterations (0.00 MB/s) against 1450-1924 MB/s on the healthy
 * links. A reboot restored it every time (proven on appmana-008 and
 * appmana-025).
 *
 * Every signal in that list is a CONTROL-plane signal (HELLO/READY over
 * XDomain ring 0) or a config-space read (paths_active() reading hop-entry
 * enable bits). None of them observes a byte crossing the DMA data ring, so
 * the driver published a dead link with total confidence. tbframe_mock's
 * ->datapath_dead models precisely that: rings allocate and start, hop entries
 * read back enabled, every control request is answered, and nothing is ever
 * delivered on the data ring.
 *
 * RED-first lever: build with -DTBFRAME_DATA_PROOF=0 (or set
 * tf.data_proof = false) to model the pre-2.46 driver. Every case below that
 * asserts "must not be declared up" then fails, because the pre-2.46 driver
 * goes UP on the control plane alone.
 */
#include <kunit/test.h>

#include "tbframe_mock.h"

static struct tbframe_mock_fixture *dp_fx(struct kunit *test)
{
	return test->priv;
}

static int dp_init(struct kunit *test)
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

static void dp_exit(struct kunit *test)
{
	tbframe_mock_fixture_exit(test, dp_fx(test));
}

/*
 * The whole point. Control plane perfect, hop entries enabled, data ring
 * silent: the link must NOT be declared up and the client must NOT be handed
 * a link_up (which is what publishes the usb4_rdmaN HCA).
 */
static void tbframe_datapath_dead_ring_never_declares_up(struct kunit *test)
{
	struct tbframe_mock_fixture *fx = dp_fx(test);
	unsigned int i;

	fx->mock.datapath_dead = true;

	/*
	 * Drive the whole probe budget. Each pass re-sends a keepalive on the
	 * data ring and re-checks; the session may never reach UP.
	 */
	for (i = 0; i < TBFRAME_PROBE_RETRIES + 2; i++)
		tbframe_link_session_step(fx->link);

	KUNIT_EXPECT_EQ(test, 0u, fx->client.up_count);
	KUNIT_EXPECT_EQ(test, 0u, fx->mock.peer_keepalives);

	/* The control plane really was healthy: HELLO and READY both landed. */
	KUNIT_EXPECT_TRUE(test, fx->mock.enable_seen);
	KUNIT_EXPECT_EQ(test, 1, fx->mock.paths_active_ret);

	/* ...and we really did try to prove it, repeatedly. */
	KUNIT_EXPECT_GE(test, fx->mock.keepalives_sent, TBFRAME_PROBE_RETRIES);
}

/*
 * Exhausting the probe budget must cycle the session hardware rather than sit
 * on a dead session: rings freed, hop entries disabled, in-HopID released. A
 * rebuild is the only in-driver repair available for this state, and it is
 * what a reboot achieves by hand.
 */
static void tbframe_datapath_dead_ring_rebuilds_hardware(struct kunit *test)
{
	struct tbframe_mock_fixture *fx = dp_fx(test);
	unsigned int i;

	fx->mock.datapath_dead = true;

	for (i = 0; i < TBFRAME_PROBE_RETRIES + 2; i++)
		tbframe_link_session_step(fx->link);

	KUNIT_EXPECT_EQ(test, 0u, fx->client.up_count);
	/* stop_rings / disable_paths / free_rings / release_in_hopid ran... */
	KUNIT_EXPECT_GE(test,
			tbframe_mock_hw_call_pos(&fx->mock,
						 TBFRAME_HW_DISABLE_PATHS, 0),
			0);
	KUNIT_EXPECT_GE(test, fx->mock.in_hopid_releases, 1u);
	/*
	 * ...and the session was rebuilt from scratch afterwards: a second
	 * in-HopID allocation means fresh rings and freshly programmed hop
	 * entries, which is the only repair the driver can attempt for this
	 * state. It still refuses to declare the link up (asserted above)
	 * because the rebuild did not fix the path either.
	 */
	KUNIT_EXPECT_GE(test, fx->mock.in_hopid_allocs, 2u);
}

/* A healthy data path is proven on the first attempt and goes up normally. */
static void tbframe_datapath_live_ring_declares_up(struct kunit *test)
{
	struct tbframe_mock_fixture *fx = dp_fx(test);

	tbframe_link_session_step(fx->link);

	KUNIT_EXPECT_EQ(test, 1u, fx->client.up_count);
	KUNIT_EXPECT_GE(test, fx->mock.peer_keepalives, 1u);
	/* One keepalive was enough; the proof must not be chatty. */
	KUNIT_EXPECT_EQ(test, 1u, fx->mock.keepalives_sent);
}

/*
 * A peer that does not negotiate keepalive cannot supply the evidence. The
 * gate must WAIVE rather than refuse the link forever -- but it must be a
 * visible waiver, not a silent one, so the link is declared up exactly once
 * and the operator has a dmesg line saying it is unverified.
 */
static void tbframe_datapath_unprovable_peer_is_waived(struct kunit *test)
{
	struct tbframe_mock_fixture *fx = dp_fx(test);

	fx->mock.datapath_dead = true;
	fx->mock.peer.capabilities = 0;	/* no CAP_KEEPALIVE */

	tbframe_link_session_step(fx->link);

	KUNIT_EXPECT_EQ(test, 1u, fx->client.up_count);
	/* Nothing was probed, because nothing could be. */
	KUNIT_EXPECT_EQ(test, 0u, fx->mock.keepalives_sent);
}

/*
 * A path that dies AFTER it was proven. paths_active() still reports the hop
 * entries enabled (paths_active_ret stays 1), which is exactly why the
 * pre-existing zombie detector cannot see this: it is the 2026-08-23 state
 * arriving at an already-established session. The level-triggered silence
 * detector must catch it and tear the session down.
 */
static void tbframe_datapath_silence_after_up_tears_down(struct kunit *test)
{
	struct tbframe_mock_fixture *fx = dp_fx(test);
	unsigned int i;

	tbframe_mock_link_up(test, fx);
	KUNIT_ASSERT_EQ(test, 1u, fx->client.up_count);

	/* The bulk path dies; the routers still say the paths are programmed. */
	fx->mock.datapath_dead = true;
	KUNIT_ASSERT_EQ(test, 1, fx->mock.paths_active_ret);

	/*
	 * One tick to take the baseline sample, then the full silent run. The
	 * detector must not fire early (asserted by
	 * tbframe_datapath_healthy_survives_verify) and must not fire late.
	 */
	for (i = 0; i < TBFRAME_DATA_SILENCE_TICKS + 1; i++)
		tbframe_link_verify_step(fx->link);

	/* needs_down was raised; run the session step that services it. */
	tbframe_link_session_step(fx->link);

	KUNIT_EXPECT_EQ(test, 1u, fx->client.down_count);
	KUNIT_EXPECT_EQ(test, (int)TBFRAME_DOWN_VERIFY,
			fx->client.reasons[fx->client.count - 1]);
}

/* A live session must never be churned by the silence detector. */
static void tbframe_datapath_healthy_survives_verify(struct kunit *test)
{
	struct tbframe_mock_fixture *fx = dp_fx(test);
	unsigned int i;

	tbframe_mock_link_up(test, fx);

	for (i = 0; i < TBFRAME_DATA_SILENCE_TICKS * 3; i++)
		tbframe_link_verify_step(fx->link);

	KUNIT_EXPECT_EQ(test, 0u, fx->client.down_count);
	KUNIT_EXPECT_EQ(test, 1u, fx->client.up_count);
}

/*
 * The proof is per-session state: a new session over freshly allocated rings
 * and freshly programmed hop entries must prove itself again, or a link that
 * was healthy once could carry a dead rebuild up.
 */
static void tbframe_datapath_proof_does_not_survive_a_session(struct kunit *test)
{
	struct tbframe_mock_fixture *fx = dp_fx(test);
	unsigned int i;

	tbframe_mock_link_up(test, fx);
	KUNIT_ASSERT_EQ(test, 1u, fx->client.up_count);

	/* Peer re-announces (supersede) AND the rebuilt path comes back dead. */
	fx->mock.datapath_dead = true;
	{
		u8 msg[TBFRAME_WIRE_HELLO_MSG_SIZE];

		KUNIT_ASSERT_GE(test,
				tbframe_mock_build_peer_msg(fx,
							    TBFRAME_WIRE_OP_HELLO,
							    msg, sizeof(msg)),
				0);
		tbframe_link_handle_packet(fx->link, msg, sizeof(msg));
	}

	for (i = 0; i < TBFRAME_PROBE_RETRIES + 3; i++)
		tbframe_link_session_step(fx->link);

	/* Down was delivered once; up must NOT have been delivered again. */
	KUNIT_EXPECT_EQ(test, 1u, fx->client.up_count);
	KUNIT_EXPECT_EQ(test, 1u, fx->client.down_count);
}

/*
 * A frame that lands during teardown must not prove the NEXT session.
 *
 * down_session resets the proof state at the top, under link->lock, but the
 * RX ring is not stopped until much later -- after the publisher drain, the
 * TX flush, quiesce_tx() and (off the shutdown path) a bounded BYE exchange.
 * Any good frame the peer delivers inside that window re-latches
 * data_proven, and nothing clears it again before the next bring_up. The
 * rebuilt session then has fresh rings, a fresh in-HopID and freshly
 * programmed hop entries, and is declared UP on evidence that belongs to the
 * session that just died -- defeating the gate precisely at the session
 * boundary it exists to guard.
 */
static void tbframe_datapath_late_frame_cannot_prove_next_session(struct kunit *test)
{
	struct tbframe_mock_fixture *fx = dp_fx(test);
	unsigned int i;

	tbframe_mock_link_up(test, fx);
	KUNIT_ASSERT_EQ(test, 1u, fx->client.up_count);

	/*
	 * The path dies, and one frame the peer had already committed to the
	 * wire arrives while we are tearing the session down.
	 */
	fx->mock.datapath_dead = true;
	fx->mock.deliver_on_quiesce = true;
	{
		u8 msg[TBFRAME_WIRE_HELLO_MSG_SIZE];

		KUNIT_ASSERT_GE(test,
				tbframe_mock_build_peer_msg(fx,
							    TBFRAME_WIRE_OP_HELLO,
							    msg, sizeof(msg)),
				0);
		tbframe_link_handle_packet(fx->link, msg, sizeof(msg));
	}

	for (i = 0; i < TBFRAME_PROBE_RETRIES + 3; i++)
		tbframe_link_session_step(fx->link);

	/*
	 * The rebuilt session moves nothing, so it must never be declared up.
	 * Before the fix the stale proof carried across and it was.
	 */
	KUNIT_EXPECT_FALSE(test, fx->link->data_proven);
	KUNIT_EXPECT_EQ(test, 1u, fx->client.up_count);
	KUNIT_EXPECT_EQ(test, 1u, fx->client.down_count);
}

static struct kunit_case tbframe_datapath_cases[] = {
	KUNIT_CASE(tbframe_datapath_dead_ring_never_declares_up),
	KUNIT_CASE(tbframe_datapath_dead_ring_rebuilds_hardware),
	KUNIT_CASE(tbframe_datapath_live_ring_declares_up),
	KUNIT_CASE(tbframe_datapath_unprovable_peer_is_waived),
	KUNIT_CASE(tbframe_datapath_silence_after_up_tears_down),
	KUNIT_CASE(tbframe_datapath_healthy_survives_verify),
	KUNIT_CASE(tbframe_datapath_proof_does_not_survive_a_session),
	KUNIT_CASE(tbframe_datapath_late_frame_cannot_prove_next_session),
	{}
};

static struct kunit_suite tbframe_datapath_suite = {
	.name = "tbframe_datapath",
	.init = dp_init,
	.exit = dp_exit,
	.test_cases = tbframe_datapath_cases,
};
kunit_test_suite(tbframe_datapath_suite);
