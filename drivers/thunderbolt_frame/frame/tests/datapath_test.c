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
	 * in-HopID allocation means restarted rings and freshly programmed hop
	 * entries, which is the only repair the driver can attempt for this
	 * state. It still refuses to declare the link up (asserted above)
	 * because the rebuild did not fix the path either.
	 */
	KUNIT_EXPECT_GE(test, fx->mock.in_hopid_allocs, 2u);
}

static void tbframe_datapath_escalates_only_after_rebuild_stays_stalled(struct kunit *test)
{
	struct tbframe_mock_fixture *fx = dp_fx(test);
	unsigned int i;

	fx->mock.datapath_dead = true;
	fx->mock.tx_consumer_stalled = true;

	for (i = 0; i < TBFRAME_PROBE_RETRIES + 2; i++)
		tbframe_link_session_step(fx->link);
	KUNIT_EXPECT_EQ(test, 0u, fx->mock.data_path_failure_reports);

	for (i = 0; i < TBFRAME_PROBE_RETRIES + 4; i++)
		tbframe_link_session_step(fx->link);
	KUNIT_EXPECT_EQ(test, 0u, fx->mock.data_path_failure_reports);

	for (i = 0; i < 2 * (TBFRAME_PROBE_RETRIES + 4); i++)
		tbframe_link_session_step(fx->link);
	KUNIT_EXPECT_EQ(test, 1u, fx->mock.data_path_failure_reports);
}

/*
 * A moving local descriptor consumer does not prove that a frame escaped the
 * router egress.  The live failure consumes every keepalive descriptor while
 * the peer receives no frame from the negotiated lifetime; rebuilding the
 * same rings and hop entries then repeats forever.  After a complete local
 * rebuild has failed too, bounded controller recovery is the only remaining
 * in-driver recovery tier and must be requested even though the descriptor
 * consumer advanced.
 */
static void tbframe_datapath_e2e_failure_escalates_with_moving_consumer(struct kunit *test)
{
	struct tbframe_mock_fixture *fx = dp_fx(test);
	unsigned int i;

	fx->mock.datapath_dead = true;

	for (i = 0; i < TBFRAME_PROBE_RETRIES + 2; i++)
		tbframe_link_session_step(fx->link);
	KUNIT_EXPECT_EQ(test, 0u, fx->mock.data_path_failure_reports);

	for (i = 0; i < TBFRAME_PROBE_RETRIES + 4; i++)
		tbframe_link_session_step(fx->link);
	KUNIT_EXPECT_EQ(test, 0u, fx->mock.data_path_failure_reports);

	for (i = 0; i < 2 * (TBFRAME_PROBE_RETRIES + 4); i++)
		tbframe_link_session_step(fx->link);
	KUNIT_EXPECT_EQ(test, 1u, fx->mock.data_path_failure_reports);
	KUNIT_EXPECT_GE(test, fx->mock.in_hopid_allocs,
			TBFRAME_AMBIGUOUS_RECOVERY_FAILURES + 1u);
	KUNIT_EXPECT_GE(test, fx->mock.in_hopid_releases,
			TBFRAME_AMBIGUOUS_RECOVERY_FAILURES);
}

/*
 * A receive-only proof must not hide a failed local transmitter.  Descriptor
 * completion only proves that the local NHI consumed a buffer; the peer must
 * authenticate receipt before the session is published.  Once a full local
 * rebuild has failed, recovery belongs to this transmitter's controller.
 */
static void tbframe_datapath_one_way_local_tx_failure_recovers_local(struct kunit *test)
{
	struct tbframe_mock_fixture *fx = dp_fx(test);
	unsigned int i;

	fx->mock.local_tx_dead = true;

	for (i = 0; i < 2 * TBFRAME_PROBE_RETRIES + 6; i++)
		tbframe_link_session_step(fx->link);

	KUNIT_EXPECT_GT(test, fx->link->data_rx, 0ull);
	KUNIT_EXPECT_EQ(test, 0u, fx->client.up_count);
	KUNIT_EXPECT_EQ(test, 1u, fx->mock.data_path_failure_reports);
}

/*
 * The mirror-image failure is not evidence against the local transmitter.
 * The peer that still receives our probes has the directional evidence needed
 * to recover itself; this receiver must rebuild without resetting the wrong
 * controller.
 */
static void tbframe_datapath_one_way_peer_tx_failure_avoids_local_recovery(struct kunit *test)
{
	struct tbframe_mock_fixture *fx = dp_fx(test);
	unsigned int i;

	fx->mock.peer_tx_dead = true;

	for (i = 0; i < 2 * TBFRAME_PROBE_RETRIES + 6; i++)
		tbframe_link_session_step(fx->link);

	KUNIT_EXPECT_EQ(test, 0u, fx->client.up_count);
	KUNIT_EXPECT_EQ(test, 0u, fx->mock.data_path_failure_reports);
}

/*
 * Frames already resident in the fabric can arrive after a session rebuild.
 * Their authenticated-but-old cookie proves that traffic is draining, not
 * that another destructive rebuild is useful.  The proof state machine must
 * wait for the negotiated lifetime without continually moving the target.
 */
static void tbframe_datapath_stale_backlog_drains_without_session_churn(struct kunit *test)
{
	struct tbframe_mock_fixture *fx = dp_fx(test);
	u64 stale_cookie = fx->mock.peer.session_cookie;
	u8 msg[TBFRAME_WIRE_HELLO_MSG_SIZE];
	unsigned int i;

	tbframe_mock_link_up(test, fx);
	fx->mock.peer.session_cookie ^= BIT_ULL(0);
	KUNIT_ASSERT_GE(test,
			tbframe_mock_build_peer_msg(fx,
						    TBFRAME_WIRE_OP_HELLO,
						    msg, sizeof(msg)),
			0);
	tbframe_link_handle_packet(fx->link, msg, sizeof(msg));
	fx->mock.datapath_dead = false;
	fx->mock.stale_peer_cookie = stale_cookie;
	fx->mock.stale_peer_keepalives = TBFRAME_PROBE_RETRIES - 2;

	for (i = 0; i < TBFRAME_PROBE_RETRIES + 4; i++)
		tbframe_link_session_step(fx->link);

	KUNIT_EXPECT_EQ(test, 2u, fx->client.up_count);
	KUNIT_EXPECT_EQ(test, 2u, fx->mock.in_hopid_allocs);
	KUNIT_EXPECT_EQ(test, 1u, fx->mock.in_hopid_releases);
}

/*
 * A recreated link has no in-memory record of cookies from before its own
 * lifetime, but the fabric can still hold frames from them.  Authenticated
 * mismatches must receive the same bounded drain allowance as a remembered
 * immediately prior cookie; the negotiated control session remains the
 * authority, and a later frame from that peer must prove the existing rings
 * without another hardware rebuild.
 */
static void tbframe_datapath_forgotten_backlog_drains_without_session_churn(struct kunit *test)
{
	struct tbframe_mock_fixture *fx = dp_fx(test);
	unsigned int i;

	fx->mock.stale_peer_cookie = fx->mock.peer.session_cookie ^ BIT_ULL(9);
	fx->mock.stale_peer_keepalives = TBFRAME_PROBE_RETRIES - 2;

	for (i = 0; i < TBFRAME_PROBE_RETRIES + 4; i++)
		tbframe_link_session_step(fx->link);

	KUNIT_EXPECT_EQ(test, 1u, fx->client.up_count);
	KUNIT_EXPECT_EQ(test, 1u, fx->mock.in_hopid_allocs);
	KUNIT_EXPECT_EQ(test, 0u, fx->mock.in_hopid_releases);
}

/*
 * Fabric residue can arrive at the peer's keepalive cadence rather than on
 * every proof probe.  Sparse authenticated stale frames still receive a
 * bounded grace period without an immediate hardware rebuild.
 */
static void tbframe_datapath_sparse_backlog_gets_drain_grace(struct kunit *test)
{
	struct tbframe_mock_fixture *fx = dp_fx(test);
	unsigned int cycle, i;

	fx->mock.datapath_dead = true;
	tbframe_link_session_step(fx->link);

	for (cycle = 0; cycle < 2; cycle++) {
		struct tbframe_frame_priv *f = tbframe_mock_pop_rx(fx);

		KUNIT_ASSERT_NOT_NULL(test, f);
		tbframe_mock_fill_keepalive(
			f, fx->mock.peer.session_cookie ^ BIT_ULL(23),
			cycle + 1, 0, 0);
		tbframe_core_rx_complete(f, false, TBFRAME_KEEPALIVE_LEN,
					 TBFRAME_PDF_KEEPALIVE, false);
		tbframe_link_session_step(fx->link);
		for (i = 0; i < TBFRAME_PROBE_RETRIES / 3; i++)
			tbframe_link_session_step(fx->link);
	}

	KUNIT_EXPECT_EQ(test, 1u, fx->mock.in_hopid_allocs);
	KUNIT_EXPECT_EQ(test, 0u, fx->mock.in_hopid_releases);
}

/*
 * A wrong identity that occupies the whole bounded drain window is not enough
 * to condemn the negotiated peer: a deep fabric backlog can do that.  A fresh
 * correlated HELLO naming the same peer must extend draining in place rather
 * than rebuild working rings.
 */
static void tbframe_datapath_persistent_mismatch_revalidates_same_peer(struct kunit *test)
{
	struct tbframe_mock_fixture *fx = dp_fx(test);
	u64 local_cookie;
	unsigned int i;

	fx->mock.datapath_dead = true;
	tbframe_link_session_step(fx->link);
	local_cookie = fx->link->local_cookie;

	for (i = 0; i < TBFRAME_PROBE_RETRIES; i++) {
		struct tbframe_frame_priv *f = tbframe_mock_pop_rx(fx);

		KUNIT_ASSERT_NOT_NULL(test, f);
		tbframe_mock_fill_keepalive(
			f, fx->mock.peer.session_cookie ^ BIT_ULL(31),
			i + 1, 0, 0);
		tbframe_core_rx_complete(f, false, TBFRAME_KEEPALIVE_LEN,
					 TBFRAME_PDF_KEEPALIVE, false);
		tbframe_link_session_step(fx->link);
	}

	KUNIT_EXPECT_FALSE(test, fx->link->needs_down);
	KUNIT_EXPECT_EQ(test, 1u, fx->mock.in_hopid_allocs);
	KUNIT_EXPECT_EQ(test, 0u, fx->mock.in_hopid_releases);
	KUNIT_EXPECT_EQ(test, local_cookie, fx->link->local_cookie);
}

/* A changed fresh HELLO confirms that the mismatch is a peer replacement. */
static void tbframe_datapath_persistent_mismatch_revalidates_changed_peer(struct kunit *test)
{
	struct tbframe_mock_fixture *fx = dp_fx(test);
	u64 local_cookie;
	unsigned int i;

	fx->mock.datapath_dead = true;
	tbframe_link_session_step(fx->link);
	local_cookie = fx->link->local_cookie;
	fx->mock.peer.session_cookie ^= BIT_ULL(37);

	for (i = 0; i < TBFRAME_PROBE_RETRIES; i++) {
		struct tbframe_frame_priv *f = tbframe_mock_pop_rx(fx);

		KUNIT_ASSERT_NOT_NULL(test, f);
		tbframe_mock_fill_keepalive(f, fx->mock.peer.session_cookie,
					 i + 1, 0, 0);
		tbframe_core_rx_complete(f, false, TBFRAME_KEEPALIVE_LEN,
					 TBFRAME_PDF_KEEPALIVE, false);
		tbframe_link_session_step(fx->link);
	}

	KUNIT_EXPECT_TRUE(test, fx->link->needs_down);
	KUNIT_EXPECT_EQ(test, (int)TBFRAME_DOWN_SUPERSEDE,
			fx->link->down_reason);
	tbframe_link_session_step(fx->link);
	KUNIT_EXPECT_EQ(test, local_cookie, fx->link->local_cookie);
}

/* An unknown cookie cannot defer recovery when control identity is unavailable. */
static void tbframe_datapath_unconfirmed_cookie_cannot_defer_recovery(struct kunit *test)
{
	struct tbframe_mock_fixture *fx = dp_fx(test);
	unsigned int i;

	fx->mock.datapath_dead = true;
	tbframe_link_session_step(fx->link);
	fx->mock.control_err = -ETIMEDOUT;

	for (i = 0; i < TBFRAME_PROBE_RETRIES; i++) {
		struct tbframe_frame_priv *f = tbframe_mock_pop_rx(fx);

		KUNIT_ASSERT_NOT_NULL(test, f);
		tbframe_mock_fill_keepalive(
			f, fx->mock.peer.session_cookie ^ BIT_ULL(0),
			i + 1, 0, 0);
		tbframe_core_rx_complete(f, false, TBFRAME_KEEPALIVE_LEN,
					 TBFRAME_PDF_KEEPALIVE, false);
		tbframe_link_session_step(fx->link);
	}

	KUNIT_EXPECT_EQ(test, 0u, fx->client.up_count);
	KUNIT_EXPECT_TRUE(test, fx->link->needs_down);
	KUNIT_EXPECT_EQ(test, (int)TBFRAME_DOWN_VERIFY,
			fx->link->down_reason);
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
	KUNIT_EXPECT_EQ(test, 1u, fx->mock.data_proven_reports);
}

static void tbframe_datapath_recovery_proof_requires_tx_and_rx(struct kunit *test)
{
	struct tbframe_mock_fixture *fx = dp_fx(test);
	struct tbframe_frame_priv *f;
	struct ring_frame *rf;

	fx->mock.tx_consumer_stalled = true;
	tbframe_link_session_step(fx->link);
	KUNIT_ASSERT_TRUE(test, fx->link->data_proven);
	KUNIT_EXPECT_EQ(test, 0u, fx->mock.data_proven_reports);

	KUNIT_ASSERT_FALSE(test, list_empty(&fx->mock.tx_queue));
	rf = list_first_entry(&fx->mock.tx_queue, struct ring_frame, list);
	list_del_init(&rf->list);
	fx->mock.tx_queued--;
	f = container_of(rf, struct tbframe_frame_priv, rf);
	tbframe_core_tx_complete(f, false);
	KUNIT_EXPECT_EQ(test, 1u, fx->mock.data_proven_reports);
}

/*
 * A peer that does not negotiate keepalive cannot supply the evidence. The
 * gate must refuse to publish the link.  An unavailable proof is not evidence
 * that data can cross the rings.
 */
static void tbframe_datapath_unprovable_peer_stays_down(struct kunit *test)
{
	struct tbframe_mock_fixture *fx = dp_fx(test);

	fx->mock.datapath_dead = true;
	fx->mock.peer.capabilities = 0;	/* no CAP_KEEPALIVE */

	tbframe_link_session_step(fx->link);

	KUNIT_EXPECT_EQ(test, 0u, fx->client.up_count);
	/* Nothing was probed, because nothing could be. */
	KUNIT_EXPECT_EQ(test, 0u, fx->mock.keepalives_sent);
}

static void tbframe_datapath_zero_length_cannot_prove(struct kunit *test)
{
	struct tbframe_mock_fixture *fx = dp_fx(test);
	struct tbframe_frame_priv *f;

	fx->mock.datapath_dead = true;
	tbframe_link_session_step(fx->link);
	f = tbframe_mock_pop_rx(fx);
	KUNIT_ASSERT_NOT_NULL(test, f);

	tbframe_core_rx_complete(f, false, 0, TBFRAME_PDF_KEEPALIVE, false);
	tbframe_link_session_step(fx->link);

	KUNIT_EXPECT_FALSE(test, fx->link->data_proven);
	KUNIT_EXPECT_EQ(test, 0u, fx->client.up_count);
}

static void tbframe_datapath_data_frame_cannot_prove_pre_up(struct kunit *test)
{
	struct tbframe_mock_fixture *fx = dp_fx(test);
	struct tbframe_frame_priv *f;

	fx->mock.datapath_dead = true;
	tbframe_link_session_step(fx->link);
	f = tbframe_mock_pop_rx(fx);
	KUNIT_ASSERT_NOT_NULL(test, f);

	tbframe_core_rx_complete(f, false, TBFRAME_KEEPALIVE_LEN,
				 TBFRAME_PDF_DATA, false);
	tbframe_link_session_step(fx->link);

	KUNIT_EXPECT_FALSE(test, fx->link->data_proven);
	KUNIT_EXPECT_EQ(test, 0u, fx->client.up_count);
}

static void tbframe_datapath_wrong_cookie_cannot_prove(struct kunit *test)
{
	struct tbframe_mock_fixture *fx = dp_fx(test);
	struct tbframe_frame_priv *f;

	fx->mock.datapath_dead = true;
	tbframe_link_session_step(fx->link);
	f = tbframe_mock_pop_rx(fx);
	KUNIT_ASSERT_NOT_NULL(test, f);

	tbframe_mock_fill_keepalive(
		f, fx->mock.peer.session_cookie ^ BIT_ULL(0), 1, 0, 0);
	tbframe_core_rx_complete(f, false, TBFRAME_KEEPALIVE_LEN,
				 TBFRAME_PDF_KEEPALIVE, false);
	tbframe_link_session_step(fx->link);

	KUNIT_EXPECT_FALSE(test, fx->link->data_proven);
	KUNIT_EXPECT_EQ(test, 0u, fx->client.up_count);
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

/* An established session must also notice that peer acknowledgements stop. */
static void tbframe_datapath_tx_silence_after_up_tears_down(struct kunit *test)
{
	struct tbframe_mock_fixture *fx = dp_fx(test);
	unsigned int i;

	tbframe_mock_link_up(test, fx);
	fx->mock.local_tx_dead = true;

	for (i = 0; i < TBFRAME_DATA_SILENCE_TICKS + 1; i++)
		tbframe_link_verify_step(fx->link);

	KUNIT_EXPECT_TRUE(test, fx->link->needs_down);
	KUNIT_EXPECT_EQ(test, (int)TBFRAME_DOWN_VERIFY,
			fx->link->down_reason);
	KUNIT_EXPECT_FALSE(test, fx->link->data_tx_proven);
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

		fx->mock.peer.session_cookie ^= BIT_ULL(7);
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
 * rebuilt session then has restarted rings, a fresh in-HopID and freshly
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

		fx->mock.peer.session_cookie ^= BIT_ULL(11);
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

static void tbframe_datapath_old_descriptor_cannot_prove_new_session(struct kunit *test)
{
	struct tbframe_mock_fixture *fx = dp_fx(test);
	struct tbframe_frame_priv *old;
	u8 msg[TBFRAME_WIRE_HELLO_MSG_SIZE];

	tbframe_mock_link_up(test, fx);
	old = tbframe_mock_pop_rx(fx);
	KUNIT_ASSERT_NOT_NULL(test, old);

	fx->mock.datapath_dead = true;
	fx->mock.peer.session_cookie ^= BIT_ULL(13);
	KUNIT_ASSERT_GE(test,
			tbframe_mock_build_peer_msg(fx, TBFRAME_WIRE_OP_HELLO,
						    msg, sizeof(msg)),
			0);
	tbframe_link_handle_packet(fx->link, msg, sizeof(msg));
	tbframe_link_session_step(fx->link);
	tbframe_link_session_step(fx->link);

	tbframe_mock_fill_keepalive(old, fx->mock.peer.session_cookie,
				1, fx->link->local_cookie,
				fx->link->keepalive_tx_seq);
	tbframe_core_rx_complete(old, false, TBFRAME_KEEPALIVE_LEN,
				 TBFRAME_PDF_KEEPALIVE, false);
	tbframe_link_session_step(fx->link);

	KUNIT_EXPECT_FALSE(test, fx->link->data_proven);
	KUNIT_EXPECT_EQ(test, 1u, fx->client.up_count);
}

static struct kunit_case tbframe_datapath_cases[] = {
	KUNIT_CASE(tbframe_datapath_dead_ring_never_declares_up),
	KUNIT_CASE(tbframe_datapath_dead_ring_rebuilds_hardware),
	KUNIT_CASE(tbframe_datapath_escalates_only_after_rebuild_stays_stalled),
	KUNIT_CASE(tbframe_datapath_e2e_failure_escalates_with_moving_consumer),
	KUNIT_CASE(tbframe_datapath_one_way_local_tx_failure_recovers_local),
	KUNIT_CASE(tbframe_datapath_one_way_peer_tx_failure_avoids_local_recovery),
	KUNIT_CASE(tbframe_datapath_stale_backlog_drains_without_session_churn),
	KUNIT_CASE(tbframe_datapath_forgotten_backlog_drains_without_session_churn),
	KUNIT_CASE(tbframe_datapath_sparse_backlog_gets_drain_grace),
	KUNIT_CASE(tbframe_datapath_persistent_mismatch_revalidates_same_peer),
	KUNIT_CASE(tbframe_datapath_persistent_mismatch_revalidates_changed_peer),
	KUNIT_CASE(tbframe_datapath_unconfirmed_cookie_cannot_defer_recovery),
	KUNIT_CASE(tbframe_datapath_live_ring_declares_up),
	KUNIT_CASE(tbframe_datapath_recovery_proof_requires_tx_and_rx),
	KUNIT_CASE(tbframe_datapath_unprovable_peer_stays_down),
	KUNIT_CASE(tbframe_datapath_zero_length_cannot_prove),
	KUNIT_CASE(tbframe_datapath_data_frame_cannot_prove_pre_up),
	KUNIT_CASE(tbframe_datapath_wrong_cookie_cannot_prove),
	KUNIT_CASE(tbframe_datapath_silence_after_up_tears_down),
	KUNIT_CASE(tbframe_datapath_healthy_survives_verify),
	KUNIT_CASE(tbframe_datapath_tx_silence_after_up_tears_down),
	KUNIT_CASE(tbframe_datapath_proof_does_not_survive_a_session),
	KUNIT_CASE(tbframe_datapath_late_frame_cannot_prove_next_session),
	KUNIT_CASE(tbframe_datapath_old_descriptor_cannot_prove_new_session),
	{}
};

static struct kunit_suite tbframe_datapath_suite = {
	.name = "tbframe_datapath",
	.init = dp_init,
	.exit = dp_exit,
	.test_cases = tbframe_datapath_cases,
};
kunit_test_suite(tbframe_datapath_suite);
