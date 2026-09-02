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
 * Admission and TX are quiesced first, but the rings must remain available
 * while the fabric drains. Stopping them first removes the descriptor base
 * that a pending hop needs to finish, turning an orderly teardown into a
 * permanent pending route. Paths therefore drain before rings stop.
 */
static void tbframe_teardown_drains_paths_before_stopping_rings(struct kunit *test)
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
	KUNIT_ASSERT_EQ(test, freed, -1);
	KUNIT_ASSERT_GE(test, release, 0);
	/* Recoverable reset: disable/drain -> stop -> release. */
	KUNIT_EXPECT_LT(test, disable, stop);
	KUNIT_EXPECT_LT(test, stop, release);
}

/*
 * Protocol-session recovery and DMA-storage lifetime are separate state
 * machines.  A recoverable session reset must stop the ring engine and paths,
 * but retaining the ring allocation keeps any device-cached descriptor or
 * buffer address valid until the link itself is removed.
 */
static void tbframe_teardown_session_reset_keeps_dma_storage(struct kunit *test)
{
	struct tbframe_mock_fixture *fx = test->priv;
	unsigned long flags;
	int first_call;

	tbframe_mock_link_up(test, fx);
	first_call = fx->mock.hw_call_count;

	spin_lock_irqsave(&fx->link->lock, flags);
	fx->link->needs_down = true;
	fx->link->down_reason = TBFRAME_DOWN_VERIFY;
	spin_unlock_irqrestore(&fx->link->lock, flags);
	tbframe_link_session_step(fx->link);

	KUNIT_EXPECT_GE(test,
		tbframe_mock_hw_call_pos(&fx->mock, TBFRAME_HW_STOP_RINGS,
					 first_call), 0);
	KUNIT_EXPECT_GE(test,
		tbframe_mock_hw_call_pos(&fx->mock, TBFRAME_HW_DISABLE_PATHS,
					 first_call), 0);
	KUNIT_EXPECT_EQ(test, -1,
		tbframe_mock_hw_call_pos(&fx->mock, TBFRAME_HW_FREE_RINGS,
					 first_call));
	KUNIT_EXPECT_TRUE(test, fx->mock.rings_alloced);
}

/*
 * Orderly teardown sequence: drain our own TX into the still-programmed
 * fabric (quiesce_tx), THEN tell the peer (BYE, while our rings still
 * absorb its residue), and only then stop rings and tear paths down.
 * Both misorders wedged a canary router egress until reboot: teardown
 * without any BYE left the surviving peer streaming into disabling hop
 * entries (validation cycle 6), and BYE before the local drain let the
 * peer kill its ingress while this side's ring still held the storm
 * backlog (validation cycle 3).
 */
static void tbframe_teardown_quiesces_then_byes_then_stops(struct kunit *test)
{
	struct tbframe_mock_fixture *fx = test->priv;
	unsigned long flags;
	int quiesce, stop;

	tbframe_mock_link_up(test, fx);
	KUNIT_ASSERT_EQ(test, 0u, fx->mock.bye_count);

	spin_lock_irqsave(&fx->link->lock, flags);
	fx->link->needs_down = true;
	fx->link->down_reason = TBFRAME_DOWN_VERIFY;
	spin_unlock_irqrestore(&fx->link->lock, flags);
	tbframe_link_session_step(fx->link);

	KUNIT_EXPECT_EQ(test, 1u, fx->mock.bye_count);
	/* Rings not yet stopped when BYE left: peer residue still lands. */
	KUNIT_EXPECT_TRUE(test, fx->mock.bye_rings_started);

	quiesce = tbframe_mock_hw_call_pos(&fx->mock, TBFRAME_HW_QUIESCE_TX, 0);
	stop = tbframe_mock_hw_call_pos(&fx->mock, TBFRAME_HW_STOP_RINGS, 0);
	KUNIT_ASSERT_GE(test, quiesce, 0);
	KUNIT_ASSERT_GE(test, stop, 0);
	/* quiesce -> BYE -> stop */
	KUNIT_EXPECT_LT(test, quiesce, (int)fx->mock.bye_hw_calls_at);
	KUNIT_EXPECT_GE(test, stop, (int)fx->mock.bye_hw_calls_at);
}

/*
 * Data-path verification runs with the hardware session active but the
 * publication state still INIT.  This is not an inactive session: both rings
 * and both paths exist and proof keepalives have already been transmitted.
 * A local verification failure must therefore fence the peer exactly like an
 * established-session failure.  Otherwise the peer keeps feeding the ingress
 * that this side is about to remove, which leaves the router egress wedged.
 */
static void
tbframe_teardown_pre_up_verify_quiesces_peer_before_paths(struct kunit *test)
{
	struct tbframe_mock_fixture *fx = test->priv;
	unsigned long flags;
	int quiesce, disable;

	fx->mock.datapath_dead = true;
	tbframe_link_session_step(fx->link);
	KUNIT_ASSERT_EQ(test, (int)TBFRAME_STATE_INIT,
			(int)fx->link->state);
	KUNIT_ASSERT_TRUE(test, fx->link->rings_up);
	KUNIT_ASSERT_TRUE(test, fx->link->paths_enabled);
	KUNIT_ASSERT_GT(test, fx->mock.keepalives_sent, 0u);

	spin_lock_irqsave(&fx->link->lock, flags);
	fx->link->needs_down = true;
	fx->link->down_reason = TBFRAME_DOWN_VERIFY;
	spin_unlock_irqrestore(&fx->link->lock, flags);
	tbframe_link_session_step(fx->link);

	KUNIT_EXPECT_EQ(test, 1u, fx->mock.bye_count);
	KUNIT_EXPECT_TRUE(test, fx->mock.bye_rings_started);
	quiesce = tbframe_mock_hw_call_pos(&fx->mock,
					  TBFRAME_HW_QUIESCE_TX, 0);
	disable = tbframe_mock_hw_call_pos(&fx->mock,
					  TBFRAME_HW_DISABLE_PATHS, 0);
	KUNIT_ASSERT_GE(test, quiesce, 0);
	KUNIT_ASSERT_GE(test, disable, 0);
	KUNIT_EXPECT_LT(test, quiesce, (int)fx->mock.bye_hw_calls_at);
	/* The next hardware event occupies the count captured at BYE time. */
	KUNIT_EXPECT_LE(test, (int)fx->mock.bye_hw_calls_at, disable);
}

/*
 * The responder side has the same independent hardware lifetime.  A BYE that
 * arrives while data proof is pending must drive that live hardware session
 * through LOGOUT; testing only the publication state loses the event and the
 * requester waits until its bounded timeout before disabling beneath us.
 */
static void tbframe_teardown_inbound_bye_quiesces_pre_up_session(struct kunit *test)
{
	struct tbframe_mock_fixture *fx = test->priv;
	u8 msg[TBFRAME_WIRE_HELLO_MSG_SIZE];

	fx->mock.datapath_dead = true;
	tbframe_link_session_step(fx->link);
	KUNIT_ASSERT_EQ(test, (int)TBFRAME_STATE_INIT,
			(int)fx->link->state);
	KUNIT_ASSERT_TRUE(test, fx->link->rings_up);
	KUNIT_ASSERT_TRUE(test, fx->link->paths_enabled);

	fx->mock.have_response = false;
	KUNIT_ASSERT_GE(test,
			tbframe_mock_build_peer_msg(fx, TBFRAME_WIRE_OP_BYE,
						    msg, sizeof(msg)), 0);
	KUNIT_EXPECT_EQ(test, 1,
			tbframe_link_handle_packet(fx->link, msg, sizeof(msg)));
	KUNIT_EXPECT_FALSE(test, fx->mock.have_response);
	flush_workqueue(fx->tf.wq);

	KUNIT_EXPECT_FALSE(test, fx->mock.rings_started);
	KUNIT_EXPECT_FALSE(test, fx->mock.paths_on);
	KUNIT_EXPECT_EQ(test, 1,
			tbframe_link_handle_packet(fx->link, msg, sizeof(msg)));
	KUNIT_EXPECT_TRUE(test, fx->mock.have_response);
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

	/*
	 * The LOGOUT re-handshake is armed with the long settle delay so the
	 * peer's teardown can finish undisturbed -- but the returning peer's
	 * fresh HELLO must collapse it IMMEDIATELY, or this side's bring_up
	 * lags the peer's by the settle and the session pairs asymmetrically
	 * (born-dead egress for the late enabler, v4 storm cycle 2). The
	 * inbound-HELLO kick must therefore preempt the armed delay.
	 */
	KUNIT_ASSERT_GE(test,
			tbframe_mock_build_peer_msg(fx, TBFRAME_WIRE_OP_HELLO,
						    msg, sizeof(msg)), 0);
	KUNIT_EXPECT_EQ(test, 1,
			tbframe_link_handle_packet(fx->link, msg, sizeof(msg)));
	flush_workqueue(fx->tf.wq);
	KUNIT_EXPECT_EQ(test, 2u, fx->client.up_count);
}

/*
 * BYE_ACK is a distributed fence: it tells the peer that this lifetime can no
 * longer put a frame on the old route.  Merely asking the TX ring to flush is
 * insufficient when a descriptor remains posted.  Keep the ACK withheld
 * until teardown has produced an observable descriptor/ring fence.
 */
static void
tbframe_teardown_bye_ack_requires_descriptor_fence(struct kunit *test)
{
	struct tbframe_mock_fixture *fx = test->priv;
	u8 msg[TBFRAME_WIRE_HELLO_MSG_SIZE];

	tbframe_mock_link_up(test, fx);
	fx->mock.hold_keepalives = true;
	tbframe_link_verify_step(fx->link);
	KUNIT_ASSERT_FALSE(test, list_empty(&fx->mock.tx_queue));

	fx->mock.have_response = false;
	KUNIT_ASSERT_GE(test,
			tbframe_mock_build_peer_msg(fx, TBFRAME_WIRE_OP_BYE,
						    msg, sizeof(msg)), 0);
	KUNIT_EXPECT_EQ(test, 1,
			tbframe_link_handle_packet(fx->link, msg, sizeof(msg)));
	KUNIT_EXPECT_FALSE(test, fx->mock.have_response);
	flush_workqueue(fx->tf.wq);

	/* Teardown cancels the descriptor before allowing the retry's ACK. */
	KUNIT_EXPECT_TRUE(test, list_empty(&fx->mock.tx_queue));
	KUNIT_EXPECT_GE(test,
			tbframe_mock_hw_call_pos(&fx->mock,
						 TBFRAME_HW_STOP_RINGS, 0), 0);
	KUNIT_EXPECT_EQ(test, 1,
			tbframe_link_handle_packet(fx->link, msg, sizeof(msg)));
	KUNIT_EXPECT_TRUE(test, fx->mock.have_response);
}

/*
 * A peer's BYE closes the old hardware lifetime before the ACK certifies that
 * no more frames can arrive from this side.  The returning HELLO therefore
 * starts from a stopped ring and removed paths rather than deferring that
 * fence until after the replacement lifetime is already negotiating.
 */
static void tbframe_teardown_bye_fences_hardware(struct kunit *test)
{
	struct tbframe_mock_fixture *fx = test->priv;
	u8 msg[TBFRAME_WIRE_HELLO_MSG_SIZE];

	tbframe_mock_link_up(test, fx);

	KUNIT_ASSERT_GE(test,
			tbframe_mock_build_peer_msg(fx, TBFRAME_WIRE_OP_BYE,
						    msg, sizeof(msg)), 0);
	KUNIT_EXPECT_EQ(test, 1,
			tbframe_link_handle_packet(fx->link, msg, sizeof(msg)));
	flush_workqueue(fx->tf.wq);
	KUNIT_ASSERT_EQ(test, 1u, fx->client.down_count);

	/* Quiesced for the client and fenced at the hardware boundary. */
	KUNIT_EXPECT_GE(test,
			tbframe_mock_hw_call_pos(&fx->mock,
						 TBFRAME_HW_QUIESCE_TX, 0), 0);
	KUNIT_EXPECT_GE(test,
			tbframe_mock_hw_call_pos(&fx->mock,
						 TBFRAME_HW_STOP_RINGS, 0), 0);
	KUNIT_EXPECT_GE(test,
			tbframe_mock_hw_call_pos(&fx->mock,
						 TBFRAME_HW_DISABLE_PATHS, 0), 0);
	KUNIT_EXPECT_TRUE(test, fx->mock.rings_alloced);

	/* The returning peer's HELLO triggers the aligned rebuild. */
	KUNIT_ASSERT_GE(test,
			tbframe_mock_build_peer_msg(fx, TBFRAME_WIRE_OP_HELLO,
						    msg, sizeof(msg)), 0);
	KUNIT_EXPECT_EQ(test, 1,
			tbframe_link_handle_packet(fx->link, msg, sizeof(msg)));
	flush_workqueue(fx->tf.wq);
	KUNIT_EXPECT_EQ(test, 2u, fx->client.up_count);
	KUNIT_EXPECT_GE(test,
			tbframe_mock_hw_call_pos(&fx->mock,
						 TBFRAME_HW_STOP_RINGS, 0), 0);
	KUNIT_EXPECT_GE(test,
			tbframe_mock_hw_call_pos(&fx->mock,
						 TBFRAME_HW_RELEASE_IN_HOPID, 0),
			0);
}

/*
 * A terminal teardown owes the client a link_down even when the session is
 * already down non-terminally. Clients keep their record (ib_device,
 * netdev) across session bounces and release it only on a terminal
 * reason; a destroy right after a peer's BYE (LOGOUT) used to skip the
 * upcall because was_up was false, so the client never unpublished --
 * every peer reboot leaked a stale usb4_rdmaN that then shadowed the live
 * device and poisoned every diagnostic run against the link.
 */
static void tbframe_teardown_terminal_down_after_logout(struct kunit *test)
{
	struct tbframe_mock_fixture *fx = test->priv;
	u8 msg[TBFRAME_WIRE_HELLO_MSG_SIZE];

	tbframe_mock_link_up(test, fx);

	/* Peer BYEs: non-terminal LOGOUT down, record must survive. */
	KUNIT_ASSERT_GE(test,
			tbframe_mock_build_peer_msg(fx, TBFRAME_WIRE_OP_BYE,
						    msg, sizeof(msg)), 0);
	KUNIT_EXPECT_EQ(test, 1,
			tbframe_link_handle_packet(fx->link, msg, sizeof(msg)));
	flush_workqueue(fx->tf.wq);
	KUNIT_ASSERT_EQ(test, 1u, fx->client.down_count);
	KUNIT_EXPECT_EQ(test, (int)TBFRAME_DOWN_LOGOUT,
			fx->client.reasons[1]);

	/* Unplug lands before any re-handshake: the client still must see
	 * the terminal reason, or its device leaks. */
	tbframe_link_destroy(fx->link, TBFRAME_DOWN_UNPLUG);
	fx->link_destroyed = true;
	KUNIT_ASSERT_EQ(test, 2u, fx->client.down_count);
	KUNIT_EXPECT_EQ(test, (int)TBFRAME_DOWN_UNPLUG,
			fx->client.reasons[2]);
	KUNIT_EXPECT_FALSE(test, fx->mock.rings_alloced);
	KUNIT_EXPECT_GE(test,
		tbframe_mock_hw_call_pos(&fx->mock, TBFRAME_HW_FREE_RINGS, 0), 0);
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

/*
 * A failed route disconnect leaves ownership of the old hardware route
 * ambiguous. The stopped rings and their HopIDs must remain pinned, and the
 * link must not approve a replacement route over that unproven lifetime.
 */
static void
tbframe_teardown_disconnect_failure_quarantines_without_reapprove(struct kunit *test)
{
	struct tbframe_mock_fixture *fx = test->priv;
	unsigned int enables, allocs, releases;
	unsigned long flags;

	tbframe_mock_link_up(test, fx);
	enables = fx->mock.enable_paths_calls;
	allocs = fx->mock.in_hopid_allocs;
	releases = fx->mock.in_hopid_releases;
	fx->mock.disable_paths_err = -ETIMEDOUT;

	spin_lock_irqsave(&fx->link->lock, flags);
	fx->link->needs_down = true;
	fx->link->down_reason = TBFRAME_DOWN_VERIFY;
	spin_unlock_irqrestore(&fx->link->lock, flags);
	tbframe_link_session_step(fx->link);

	KUNIT_EXPECT_EQ(test, (int)TBFRAME_STATE_DEAD,
			(int)fx->link->state);
	KUNIT_EXPECT_FALSE(test, fx->mock.rings_started);
	KUNIT_EXPECT_TRUE(test, fx->mock.rings_alloced);
	KUNIT_EXPECT_TRUE(test, fx->mock.paths_on);
	KUNIT_EXPECT_EQ(test, releases, fx->mock.in_hopid_releases);
	KUNIT_EXPECT_EQ(test, allocs, fx->mock.in_hopid_allocs);
	KUNIT_EXPECT_EQ(test, enables, fx->mock.enable_paths_calls);
	KUNIT_EXPECT_EQ(test, 1u, fx->mock.quarantine_requests);
	KUNIT_EXPECT_TRUE(test, fx->link->terminal_handoff);
	KUNIT_EXPECT_FALSE(test, fx->link->up_delivered);
	KUNIT_EXPECT_FALSE(test, fx->mock.freed_rings_while_paths_on);
	KUNIT_EXPECT_FALSE(test, fx->mock.released_hopid_while_paths_on);
	KUNIT_ASSERT_EQ(test, 1u, fx->client.down_count);
	KUNIT_EXPECT_EQ(test, (int)TBFRAME_DOWN_DEAD_HW,
			fx->client.reasons[1]);

	/* A later session step may not allocate or approve over the old route. */
	tbframe_link_session_step(fx->link);
	KUNIT_EXPECT_EQ(test, enables, fx->mock.enable_paths_calls);
	KUNIT_EXPECT_EQ(test, allocs, fx->mock.in_hopid_allocs);
	KUNIT_EXPECT_EQ(test, releases, fx->mock.in_hopid_releases);
}

/*
 * An activation error does not prove that activation left no hardware behind.
 * If the core retained a tunnel because its rollback could not drain a hop,
 * the exact rings and HopID named by that tunnel must remain owned.  A retry
 * must first finish disconnecting that tuple; it may not free it and allocate
 * a replacement over the ambiguous route.
 */
static void
tbframe_activation_failure_quarantines_ambiguous_ownership(struct kunit *test)
{
	struct tbframe_mock_fixture *fx = test->priv;
	unsigned int allocs, releases;

	fx->mock.enable_paths_err = -EUCLEAN;
	fx->mock.disable_paths_err = -ETIMEDOUT;
	allocs = fx->mock.in_hopid_allocs;
	releases = fx->mock.in_hopid_releases;

	tbframe_link_session_step(fx->link);

	KUNIT_EXPECT_EQ(test, 1u, fx->mock.enable_paths_calls);
	KUNIT_EXPECT_EQ(test, 1u, fx->mock.disable_paths_calls);
	KUNIT_EXPECT_EQ(test, (int)TBFRAME_STATE_DEAD,
			(int)fx->link->state);
	KUNIT_EXPECT_TRUE(test, fx->mock.rings_alloced);
	KUNIT_EXPECT_TRUE(test, fx->mock.paths_on);
	KUNIT_EXPECT_EQ(test, allocs + 1, fx->mock.in_hopid_allocs);
	KUNIT_EXPECT_EQ(test, releases, fx->mock.in_hopid_releases);
	KUNIT_EXPECT_EQ(test, 1u, fx->mock.quarantine_requests);
	KUNIT_EXPECT_TRUE(test, fx->link->terminal_handoff);
	KUNIT_EXPECT_FALSE(test, fx->mock.freed_rings_while_paths_on);
	KUNIT_EXPECT_FALSE(test, fx->mock.released_hopid_while_paths_on);

	/* A scheduled retry must not allocate or approve a replacement tuple. */
	tbframe_link_session_step(fx->link);
	KUNIT_EXPECT_EQ(test, 1u, fx->mock.enable_paths_calls);
	KUNIT_EXPECT_EQ(test, allocs + 1, fx->mock.in_hopid_allocs);
	KUNIT_EXPECT_EQ(test, releases, fx->mock.in_hopid_releases);
}

/*
 * A nonterminal rebuild keeps the client device published between sessions.
 * If the next activation cannot roll back, the core recovery handoff becomes
 * terminal and must withdraw that previously published device exactly once.
 */
static void
tbframe_activation_quarantine_withdraws_prior_publication(struct kunit *test)
{
	struct tbframe_mock_fixture *fx = test->priv;
	unsigned long flags;

	tbframe_mock_link_up(test, fx);
	spin_lock_irqsave(&fx->link->lock, flags);
	fx->link->needs_down = true;
	fx->link->down_reason = TBFRAME_DOWN_VERIFY;
	spin_unlock_irqrestore(&fx->link->lock, flags);
	tbframe_link_session_step(fx->link);
	KUNIT_ASSERT_EQ(test, 1u, fx->client.down_count);
	KUNIT_ASSERT_TRUE(test, fx->link->up_delivered);

	fx->mock.enable_paths_err = -EUCLEAN;
	fx->mock.disable_paths_err = -ETIMEDOUT;
	tbframe_link_session_step(fx->link);

	KUNIT_EXPECT_EQ(test, (int)TBFRAME_STATE_DEAD,
			(int)fx->link->state);
	KUNIT_EXPECT_TRUE(test, fx->link->terminal_handoff);
	KUNIT_EXPECT_FALSE(test, fx->link->up_delivered);
	KUNIT_EXPECT_EQ(test, 2u, fx->client.down_count);
	KUNIT_EXPECT_EQ(test, (int)TBFRAME_DOWN_DEAD_HW,
			fx->client.reasons[2]);
}

/*
 * Terminal removal has the same ownership rule. If the route cannot be
 * disconnected, retain every object that its DMA programming may still name
 * and complete only after the core has accepted the exact ownership handoff.
 */
static void
tbframe_teardown_disconnect_failure_leaks_owned_dma_state(struct kunit *test)
{
	struct tbframe_mock_fixture *fx = test->priv;
	int ret;

	tbframe_mock_link_up(test, fx);
	fx->mock.disable_paths_err = -ETIMEDOUT;

	ret = tbframe_link_destroy(fx->link, TBFRAME_DOWN_UNPLUG);
	fx->link_destroyed = true;

	KUNIT_EXPECT_EQ(test, 0, ret);
	KUNIT_EXPECT_EQ(test, 1u, fx->mock.quarantine_requests);
	KUNIT_EXPECT_TRUE(test, fx->mock.rings_quarantined);
	KUNIT_EXPECT_TRUE(test, fx->mock.rings_alloced);
	KUNIT_EXPECT_TRUE(test, fx->mock.paths_on);
	KUNIT_EXPECT_EQ(test, 0u, fx->mock.in_hopid_releases);
	KUNIT_EXPECT_EQ(test, 0u, fx->mock.out_hopid_releases);
	KUNIT_EXPECT_FALSE(test, fx->mock.freed_rings_while_paths_on);
	KUNIT_EXPECT_FALSE(test, fx->mock.released_hopid_while_paths_on);
	KUNIT_ASSERT_EQ(test, 1u, fx->client.down_count);
	KUNIT_EXPECT_EQ(test, (int)TBFRAME_DOWN_DEAD_HW,
			fx->client.reasons[1]);
}

/*
 * Operationally inactive is not the same as fully drained. A partial
 * teardown can leave one hop disabled but pending, or disable one direction
 * while the other is still enabled. A generic "not active" result may not
 * turn a disconnect error into permission to release DMA ownership.
 */
static void
tbframe_teardown_inactive_readback_does_not_prove_drained(struct kunit *test)
{
	struct tbframe_mock_fixture *fx = test->priv;
	int ret;

	tbframe_mock_link_up(test, fx);
	fx->mock.disable_paths_err = -ETIMEDOUT;
	fx->mock.paths_active_ret = 0;

	ret = tbframe_link_destroy(fx->link, TBFRAME_DOWN_UNPLUG);
	fx->link_destroyed = true;

	KUNIT_EXPECT_EQ(test, 0, ret);
	KUNIT_EXPECT_EQ(test, 1u, fx->mock.quarantine_requests);
	KUNIT_EXPECT_EQ(test, 0u, fx->mock.paths_active_calls);
	KUNIT_EXPECT_TRUE(test, fx->mock.rings_alloced);
	KUNIT_EXPECT_EQ(test, 0u, fx->mock.in_hopid_releases);
	KUNIT_EXPECT_EQ(test, 0u, fx->mock.out_hopid_releases);
	KUNIT_EXPECT_FALSE(test, fx->mock.freed_rings_while_paths_on);
	KUNIT_EXPECT_FALSE(test, fx->mock.released_hopid_while_paths_on);
	KUNIT_ASSERT_EQ(test, 1u, fx->client.down_count);
	KUNIT_EXPECT_EQ(test, (int)TBFRAME_DOWN_DEAD_HW,
			fx->client.reasons[1]);
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
	KUNIT_CASE(tbframe_teardown_drains_paths_before_stopping_rings),
	KUNIT_CASE(tbframe_teardown_session_reset_keeps_dma_storage),
	KUNIT_CASE(tbframe_teardown_quiesces_then_byes_then_stops),
	KUNIT_CASE(tbframe_teardown_pre_up_verify_quiesces_peer_before_paths),
	KUNIT_CASE(tbframe_teardown_inbound_bye_quiesces_pre_up_session),
	KUNIT_CASE(tbframe_teardown_inbound_bye_quiesces_then_acks),
	KUNIT_CASE(tbframe_teardown_bye_ack_requires_descriptor_fence),
	KUNIT_CASE(tbframe_teardown_bye_fences_hardware),
	KUNIT_CASE(tbframe_teardown_terminal_down_after_logout),
	KUNIT_CASE(tbframe_teardown_publisher_drain_poisons),
	KUNIT_CASE(tbframe_teardown_disconnect_failure_quarantines_without_reapprove),
	KUNIT_CASE(tbframe_activation_failure_quarantines_ambiguous_ownership),
	KUNIT_CASE(tbframe_activation_quarantine_withdraws_prior_publication),
	KUNIT_CASE(tbframe_teardown_disconnect_failure_leaks_owned_dma_state),
	KUNIT_CASE(tbframe_teardown_inactive_readback_does_not_prove_drained),
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
