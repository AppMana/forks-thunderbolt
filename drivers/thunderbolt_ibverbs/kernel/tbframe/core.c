// SPDX-License-Identifier: GPL-2.0
/*
 * tbframe core: session state machine, Mode A admission, frame pools and
 * bounded teardown. No tb_* symbol is referenced here; all hardware access
 * goes through struct tbframe_hw_ops (hw.c in production, a mock in KUnit).
 *
 * Locking:
 *  - link->session_lock (mutex) serializes the session state machine:
 *    session work, down-session and destroy. No downcall takes it, so it
 *    may be held across link_up()/link_down() to keep those ordered.
 *  - link->lock (spinlock) covers state, admission counters, frame lists
 *    and every work/timer re-queue gate (checked against ->removing under
 *    the lock, the mainline 2c5d2d3c3f70 pattern).
 *  - tf->lock (mutex) covers the link list; always taken outside
 *    session_lock.
 *  - rx()/tx_released() upcalls run with no tbframe lock held (only the
 *    client rwsem read side, which no downcall takes).
 */

#define pr_fmt(fmt) "tbframe: " fmt

#include <linux/errno.h>
#include <linux/iopoll.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/random.h>
#include <linux/slab.h>

#include "tbframe_priv.h"

static void tbframe_session_workfn(struct work_struct *work);
static void tbframe_tx_released_workfn(struct work_struct *work);
static void tbframe_verify_workfn(struct work_struct *work);

void tbframe_state_init(struct tbframe *tf)
{
	mutex_init(&tf->lock);
	INIT_LIST_HEAD(&tf->links);
	init_rwsem(&tf->client_rwsem);
	tf->client_ops = NULL;
	tf->client_ctx = NULL;
}

static void tbframe_link_put(struct tbframe_link *link)
{
	if (refcount_dec_and_test(&link->refcnt))
		complete(&link->refs_zero);
}

/*
 * Queue the session work honoring every re-arm gate. Lock held.
 *
 * This is the ONLY place the session work is queued, so the gates below are
 * the complete set of conditions under which tbframe can re-enter its state
 * machine (the mainline 2c5d2d3c3f70 "removing flag checked under the lock
 * at every queue site" discipline). ->parked is what makes the client's
 * link_down the last upcall it can ever see for a link.
 */
static void tbframe_link_kick_locked(struct tbframe_link *link,
				     unsigned long delay)
{
	lockdep_assert_held(&link->lock);
	if (link->removing || link->parked ||
	    link->state == TBFRAME_STATE_DEAD)
		return;
	/*
	 * Earliest deadline wins. queue_delayed_work() is a no-op on a
	 * pending item, so an immediate kick (inbound HELLO from a peer
	 * that just came back) could not preempt a long-armed retry --
	 * measured on the canaries as the LOGOUT settle holding through a
	 * returning peer's HELLO, a 4.7 s one-sided paths-enable window,
	 * and a born-dead session (the late enabler's egress never gets
	 * credits). mod_delayed_work() for delay 0 pulls the work forward;
	 * non-zero kicks keep the no-op-if-pending semantics so a retry
	 * can never push an imminent run back.
	 */
	if (!delay)
		mod_delayed_work(link->tf->wq, &link->session_work, 0);
	else
		queue_delayed_work(link->tf->wq, &link->session_work, delay);
}

static void tbframe_link_kick(struct tbframe_link *link, unsigned long delay)
{
	unsigned long flags;

	spin_lock_irqsave(&link->lock, flags);
	tbframe_link_kick_locked(link, delay);
	spin_unlock_irqrestore(&link->lock, flags);
}

/* Mode A window (spec §6): min of the two ring caps minus the reserve. */
static u16 tbframe_link_window(const struct tbframe_link *link)
{
	u32 cap = min_t(u32, link->tf->ring_entries, link->remote_rx_entries);

	if (cap <= TBFRAME_CTRL_RESERVE + 1)
		return 1;
	return cap - TBFRAME_CTRL_RESERVE;
}

static void tbframe_link_fill_info_locked(const struct tbframe_link *link,
					  struct tbframe_link_info *info)
{
	memset(info, 0, sizeof(*info));
	info->gid_eui64 = link->remote_gid_eui64;
	info->local_gid_eui64 = link->local_gid_eui64;
	info->rx_ring_entries = link->remote_rx_entries;
	info->data_window = link->data_window;
	info->max_payload = TBFRAME_MAX_FRAME;
	info->e2e = link->e2e_active;
}

/* Callers must not hold link->lock; remote fields change under it. */
static void tbframe_link_apply_remote_locked(struct tbframe_link *link,
					     const struct tbframe_wire_hello *h)
{
	lockdep_assert_held(&link->lock);
	link->remote_hopid = h->transmit_hopid;
	link->remote_rx_entries = clamp_t(u16, h->rx_ring_entries, 1, 4096);
	link->remote_caps = h->capabilities;
	link->remote_gid_eui64 = h->gid_eui64;
	link->remote_cookie = h->session_cookie;
}

static void tbframe_link_fill_local_hello(struct tbframe_link *link,
					  struct tbframe_wire_hello *hello)
{
	struct tbframe *tf = link->tf;

	memset(hello, 0, sizeof(*hello));
	hello->proto_version = TBFRAME_WIRE_VERSION;
	hello->transmit_hopid = link->local_hopid;
	hello->rx_ring_entries = tf->ring_entries;
	if (tf->e2e)
		hello->capabilities |= TBFRAME_WIRE_CAP_E2E;
	if (tf->keepalive)
		hello->capabilities |= TBFRAME_WIRE_CAP_KEEPALIVE;
	hello->gid_eui64 = link->local_gid_eui64;
	hello->session_cookie = link->local_cookie;
}

/*
 * Rate-limited property re-announce: an unanswered HELLO usually means the
 * peer's one-shot property read raced our module load, so it never created
 * our tbframe service and has no link to answer from (the legacy driver's
 * "1-in-10 link survival" fix). Retry forever; convergence beats budgets.
 */
static void tbframe_link_reannounce(struct tbframe_link *link)
{
	unsigned long last = READ_ONCE(link->last_reannounce);

	if (last && time_before(jiffies,
				last + msecs_to_jiffies(TBFRAME_REANNOUNCE_MIN_MS)))
		return;
	WRITE_ONCE(link->last_reannounce, jiffies);
	if (link->ops->reannounce)
		link->ops->reannounce(link->hw);
}

static int tbframe_link_hello_once(struct tbframe_link *link)
{
	u8 req[TBFRAME_WIRE_HELLO_MSG_SIZE];
	u8 resp[TBFRAME_WIRE_HELLO_MSG_SIZE];
	struct tbframe_wire_hello local;
	struct tbframe_wire_hello remote;
	struct tbframe_wire_info info;
	unsigned long flags;
	int ret;

	tbframe_link_fill_local_hello(link, &local);
	ret = tbframe_wire_build_hello(req, sizeof(req), &local,
				       TBFRAME_WIRE_OP_HELLO, 0, link->route,
				       link->local_hopid & 0x3);
	if (ret < 0)
		return ret;

	memset(resp, 0, sizeof(resp));
	ret = link->ops->control_request(link->hw, req, sizeof(req), resp,
					 sizeof(resp),
					 TBFRAME_HELLO_TIMEOUT_MS);
	if (ret)
		return ret;

	ret = tbframe_wire_parse_hello(resp, sizeof(resp), &remote, &info);
	if (ret)
		return ret;
	if (info.op != TBFRAME_WIRE_OP_HELLO_ACK)
		return -EPROTO;
	if (remote.proto_version != TBFRAME_WIRE_VERSION) {
		pr_warn_ratelimited("%s: peer speaks tbframe v%u, this module speaks v%u; refusing session\n",
				    link->name, remote.proto_version,
				    TBFRAME_WIRE_VERSION);
		return -EPROTO;
	}

	spin_lock_irqsave(&link->lock, flags);
	tbframe_link_apply_remote_locked(link, &remote);
	link->hello_done = true;
	spin_unlock_irqrestore(&link->lock, flags);

	pr_info("%s: HELLO negotiated remote_hopid=%u rx_entries=%u caps=0x%x\n",
		link->name, remote.transmit_hopid, remote.rx_ring_entries,
		remote.capabilities);
	return 0;
}

static void tbframe_frame_putback_rx(struct tbframe_frame_priv *f)
{
	struct tbframe_link *link = f->link;
	unsigned long flags;

	spin_lock_irqsave(&link->lock, flags);
	list_add_tail(&f->node, &link->rx_free);
	spin_unlock_irqrestore(&link->lock, flags);
	tbframe_link_put(link);
}

/* Repost an RX descriptor, or return it to the pool if the session is down. */
static void tbframe_frame_recycle_rx(struct tbframe_frame_priv *f)
{
	struct tbframe_link *link = f->link;
	unsigned long flags;
	bool repost;

	atomic_inc(&link->hw_active);
	spin_lock_irqsave(&link->lock, flags);
	repost = link->rings_up && !link->removing;
	spin_unlock_irqrestore(&link->lock, flags);
	if (repost && !link->ops->post_rx(link->hw, f)) {
		atomic_dec(&link->hw_active);
		return;
	}
	atomic_dec(&link->hw_active);
	tbframe_frame_putback_rx(f);
}

/*
 * Bring up the data plane, tbnet_connected_work ordering: allocate rings,
 * register the in-HopID from the peer's advertised transmit HopID, start
 * the rings, prime every RX descriptor, and only then enable the paths so
 * no frame can arrive before its descriptor exists.
 */
static int tbframe_link_bring_up(struct tbframe_link *link)
{
	struct tbframe *tf = link->tf;
	unsigned long flags;
	u16 remote_hopid;
	bool e2e;
	int ret;

	spin_lock_irqsave(&link->lock, flags);
	remote_hopid = link->remote_hopid;
	/* Mode B only when both HELLOs advertised capability bit 0. */
	e2e = tf->e2e && (link->remote_caps & TBFRAME_WIRE_CAP_E2E);
	spin_unlock_irqrestore(&link->lock, flags);

	ret = link->ops->alloc_rings(link->hw, tf->ring_entries,
				     tf->ring_entries, e2e);
	if (ret)
		return ret;

	ret = link->ops->alloc_in_hopid(link->hw, remote_hopid);
	if (ret < 0)
		goto err_free_rings;
	/*
	 * Record what we actually allocated. Teardown must release THIS
	 * HopID: an inbound HELLO can rewrite link->remote_hopid from the
	 * dispatch context between here and down_session, and releasing a
	 * HopID that was never allocated both splats ida_free() and leaks
	 * the real one -- after which the next session's alloc_in_hopid()
	 * fails -EBUSY and the link never comes back.
	 */
	spin_lock_irqsave(&link->lock, flags);
	link->active_remote_hopid = remote_hopid;
	link->in_hopid_held = true;
	spin_unlock_irqrestore(&link->lock, flags);

	link->ops->start_rings(link->hw);

	for (;;) {
		struct tbframe_frame_priv *f;

		spin_lock_irqsave(&link->lock, flags);
		f = list_first_entry_or_null(&link->rx_free,
					     struct tbframe_frame_priv, node);
		if (f) {
			list_del_init(&f->node);
			refcount_inc(&link->refcnt);
		}
		spin_unlock_irqrestore(&link->lock, flags);
		if (!f)
			break;
		ret = link->ops->post_rx(link->hw, f);
		if (ret) {
			tbframe_frame_putback_rx(f);
			goto err_stop_rings;
		}
	}

	ret = link->ops->enable_paths(link->hw, link->local_hopid,
				      remote_hopid);
	if (ret)
		goto err_stop_rings;

	spin_lock_irqsave(&link->lock, flags);
	link->rings_up = true;
	link->paths_enabled = true;
	link->e2e_active = e2e;
	link->data_window = tbframe_link_window(link);
	spin_unlock_irqrestore(&link->lock, flags);

	pr_info("%s: paths enabled local_hopid=%d remote_hopid=%u window=%u e2e=%u\n",
		link->name, link->local_hopid, remote_hopid,
		link->data_window, e2e);
	return 0;

err_stop_rings:
	/* Cancels every posted RX frame back to the pool via the callbacks. */
	link->ops->stop_rings(link->hw);
err_free_rings:
	link->ops->free_rings(link->hw);
	spin_lock_irqsave(&link->lock, flags);
	if (link->in_hopid_held) {
		link->in_hopid_held = false;
		spin_unlock_irqrestore(&link->lock, flags);
		link->ops->release_in_hopid(link->hw, remote_hopid);
	} else {
		spin_unlock_irqrestore(&link->lock, flags);
	}
	return ret;
}

static int tbframe_link_ready_once(struct tbframe_link *link)
{
	u8 req[TBFRAME_WIRE_HELLO_MSG_SIZE];
	u8 resp[TBFRAME_WIRE_HELLO_MSG_SIZE];
	struct tbframe_wire_hello local;
	struct tbframe_wire_hello remote;
	struct tbframe_wire_info info;
	unsigned long flags;
	int ret;

	tbframe_link_fill_local_hello(link, &local);
	ret = tbframe_wire_build_hello(req, sizeof(req), &local,
				       TBFRAME_WIRE_OP_READY, 0, link->route,
				       link->local_hopid & 0x3);
	if (ret < 0)
		return ret;

	memset(resp, 0, sizeof(resp));
	ret = link->ops->control_request(link->hw, req, sizeof(req), resp,
					 sizeof(resp),
					 TBFRAME_HELLO_TIMEOUT_MS);
	if (ret)
		return ret;

	ret = tbframe_wire_parse_hello(resp, sizeof(resp), &remote, &info);
	if (ret)
		return ret;
	if (info.op != TBFRAME_WIRE_OP_READY_ACK)
		return -EPROTO;

	/*
	 * A peer only acks READY once its own paths are enabled (the inbound
	 * handler withholds the ack until then), so the ack carries the same
	 * evidence as an inbound READY: count it as peer_seen too.
	 */
	spin_lock_irqsave(&link->lock, flags);
	link->hs.request_sent = true;
	link->hs.peer_seen = true;
	spin_unlock_irqrestore(&link->lock, flags);
	pr_info("%s: READY confirmed\n", link->name);
	return 0;
}

/*
 * Orderly-teardown quiesce (tbnet LOGOUT analog). The canary campaign
 * measured that a peer which keeps streaming into paths we are about to
 * tear down wedges ITS router egress persistently (reset-only recovery):
 * validation cycle 6, 023 reloading while 025 streamed, 025's TX ring
 * frozen at zero consumption afterwards. So before this side touches its
 * hop entries, tell the peer; the peer downs its session (admission
 * closed, rings cancelled) and acks only once it has left UP. Bounded:
 * a dead or pre-BYE peer costs TBFRAME_BYE_RETRIES * TBFRAME_BYE_TIMEOUT_MS
 * and teardown proceeds exactly as before.
 */
static void tbframe_link_bye(struct tbframe_link *link)
{
	u8 req[TBFRAME_WIRE_HELLO_MSG_SIZE];
	u8 resp[TBFRAME_WIRE_HELLO_MSG_SIZE];
	struct tbframe_wire_hello local;
	struct tbframe_wire_hello remote;
	struct tbframe_wire_info info;
	unsigned int i;
	int ret;

	tbframe_link_fill_local_hello(link, &local);
	ret = tbframe_wire_build_hello(req, sizeof(req), &local,
				       TBFRAME_WIRE_OP_BYE, 0, link->route,
				       link->local_hopid & 0x3);
	if (ret < 0)
		return;

	for (i = 0; i < TBFRAME_BYE_RETRIES; i++) {
		memset(resp, 0, sizeof(resp));
		ret = link->ops->control_request(link->hw, req, sizeof(req),
						 resp, sizeof(resp),
						 TBFRAME_BYE_TIMEOUT_MS);
		if (ret)
			continue;
		if (!tbframe_wire_parse_hello(resp, sizeof(resp), &remote,
					      &info) &&
		    info.op == TBFRAME_WIRE_OP_BYE_ACK)
			return;
	}
	pr_info("%s: BYE unacknowledged; proceeding with teardown\n",
		link->name);
}

static void tbframe_link_deliver_up(struct tbframe_link *link,
				    struct tbframe_link_info *info)
{
	struct tbframe *tf = link->tf;

	if (link->ops->link_attrs)
		link->ops->link_attrs(link->hw, &info->width, &info->speed);
	if (link->ops->peer_identity)
		link->ops->peer_identity(link->hw, info->remote_uuid,
					 info->remote_name,
					 sizeof(info->remote_name));
	down_read(&tf->client_rwsem);
	if (tf->client_ops && tf->client_ops->link_up)
		tf->client_ops->link_up(tf->client_ctx, link, info);
	up_read(&tf->client_rwsem);
}

static void tbframe_link_maybe_up(struct tbframe_link *link)
{
	struct tbframe_link_info info;
	unsigned long flags;
	bool deliver = false;

	spin_lock_irqsave(&link->lock, flags);
	/*
	 * !needs_down: a supersede can land between this step's down-check
	 * snapshot and here (inbound HELLO from the dispatch context while
	 * bring_up was in flight). Announcing UP over a session that is
	 * already queued for teardown hands the client a link whose paths
	 * are about to vanish; let the down run and the re-handshake deliver
	 * the up.
	 */
	if (link->state == TBFRAME_STATE_INIT && !link->removing &&
	    !link->needs_down &&
	    link->hello_done && link->rings_up && link->paths_enabled &&
	    tb_xdomain_handshake_complete(&link->hs)) {
		link->state = TBFRAME_STATE_UP;
		link->hs.established = true;
		/*
		 * Data can flow from here on; until now the READY gate kept
		 * the fresh rings silent, so a BYE_ACK (which certifies TX
		 * silence) stays legitimate through bring_up and handshake.
		 */
		link->tx_quiesced = false;
		link->tx_blocked = false;
		link->announce_pending = false;
		tbframe_link_fill_info_locked(link, &info);
		deliver = true;
	} else if (link->state == TBFRAME_STATE_UP && link->announce_pending) {
		/* A client registered after this link was already up. */
		link->announce_pending = false;
		tbframe_link_fill_info_locked(link, &info);
		deliver = true;
	}
	spin_unlock_irqrestore(&link->lock, flags);

	if (!deliver)
		return;

	pr_info("%s: link up\n", link->name);
	tbframe_link_deliver_up(link, &info);

	spin_lock_irqsave(&link->lock, flags);
	link->up_delivered = true;
	if (!link->removing && link->state == TBFRAME_STATE_UP)
		mod_timer(&link->verify_timer,
			  jiffies + msecs_to_jiffies(link->tf->verify_ms));
	spin_unlock_irqrestore(&link->lock, flags);
}

/*
 * Tear the session down to the negotiation floor. session_lock held.
 * Recovery never escalates past this re-handshake: no core unload, no NHI
 * reset. The only unbounded thing below us is hardware that never cancels;
 * every wait of our own is bounded and poisons the link on expiry.
 */
static void tbframe_link_down_session(struct tbframe_link *link,
				      enum tbframe_down_reason reason)
{
	struct tbframe *tf = link->tf;
	unsigned long flags;
	bool was_up, deliver_down, terminal, had_rings, had_paths, had_hopid;
	u16 remote_hopid;
	int active;

	lockdep_assert_held(&link->session_lock);

	spin_lock_irqsave(&link->lock, flags);
	was_up = link->state == TBFRAME_STATE_UP;
	if (link->state != TBFRAME_STATE_DEAD)
		link->state = TBFRAME_STATE_INIT;
	had_rings = link->rings_up;
	had_paths = link->paths_enabled;
	had_hopid = link->in_hopid_held;
	/*
	 * NOT link->remote_hopid: that field tracks the peer's latest HELLO
	 * and an inbound HELLO can move it from the dispatch context while
	 * this session is up. Undo exactly what bring_up did.
	 */
	remote_hopid = link->active_remote_hopid;
	link->rings_up = false;
	link->paths_enabled = false;
	link->in_hopid_held = false;
	link->hello_done = false;
	link->needs_down = false;
	link->tx_blocked = false;
	link->hello_attempts = 0;
	tb_xdomain_handshake_reset(&link->hs);
	spin_unlock_irqrestore(&link->lock, flags);

	timer_delete_sync(&link->verify_timer);

	/*
	 * Admission is closed above; wait out the publishers that already
	 * passed their state check and are inside ring_tx()/post_rx().
	 * Bounded: hardware that wedges a publisher poisons the link.
	 */
	if (read_poll_timeout(atomic_read, active, !active, 100,
			      (u64)tf->xmit_drain_ms * 1000, false,
			      &link->hw_active)) {
		spin_lock_irqsave(&link->lock, flags);
		link->state = TBFRAME_STATE_DEAD;
		spin_unlock_irqrestore(&link->lock, flags);
		pr_err("%s: publisher drain timed out (%d active); poisoning link DEAD_HW\n",
		       link->name, atomic_read(&link->hw_active));
		reason = TBFRAME_DOWN_DEAD_HW;
	}

	/*
	 * Drain our TX into the still fully-programmed fabric FIRST -- both
	 * paths and the peer's ingress are untouched at this point, so the
	 * backlog has somewhere to go -- and only then mark this side
	 * quiesced and tell the peer. Ordering is the whole point: a router
	 * left holding frames for a path that loses its far end wedges its
	 * egress until a reset (canary cycle 3: the receiver acked BYE and
	 * killed its ingress while the sender's ring still held the storm
	 * backlog; the flush then had nowhere to drain).
	 */
	if (had_rings && link->ops->quiesce_tx)
		link->ops->quiesce_tx(link->hw);
	spin_lock_irqsave(&link->lock, flags);
	link->tx_quiesced = true;
	spin_unlock_irqrestore(&link->lock, flags);

	/*
	 * BYE after our own quiesce (the ack contract is symmetric: each
	 * side certifies its TX is silent), before any path teardown. Only
	 * for downs the peer cannot already know about (local close,
	 * dead-path verify): SUPERSEDE and LOGOUT are peer-initiated, on
	 * UNPLUG the control channel is gone, and a DEAD_HW-poisoned link
	 * skips it. The peer downs its session on BYE and acks once its own
	 * TX is quiesced; only after that (or the bounded budget) do we
	 * disable the paths its flush drains into.
	 */
	if (was_up && (reason == TBFRAME_DOWN_CLOSED ||
		       reason == TBFRAME_DOWN_VERIFY))
		tbframe_link_bye(link);

	/*
	 * Quiesce the NHI before touching the fabric, the tbnet_tear_down()
	 * order: stop the rings (cancelling whatever the flush above could
	 * not drain), and only then disable the hop entries. The old order
	 * deactivated the egress hop entry under a still actively-DMAing TX
	 * ring -- the local half of the rapid-disable-of-an-active-path
	 * pattern behind the router-level egress wedge.
	 */
	if (had_rings)
		/* Cancels all in-flight frames through the completion path. */
		link->ops->stop_rings(link->hw);
	if (had_paths)
		link->ops->disable_paths(link->hw, link->local_hopid,
					 remote_hopid);
	if (had_rings)
		link->ops->free_rings(link->hw);
	if (had_hopid)
		link->ops->release_in_hopid(link->hw, remote_hopid);

	/*
	 * The transmit HopID is deliberately NOT rotated across sessions.
	 * Rotation was tried as hygiene against per-HopID router leftovers
	 * and measured on the 023/025 canaries (2026-08-18): it does not
	 * recover the egress wedge -- the poison is per-port/link (lane
	 * adapter egress credit state, cleared only by a router reset), a
	 * session on a fresh HopID stayed just as dead -- and it
	 * destabilizes re-negotiation: a peer whose down_session rotates
	 * mid-handshake lands its new HopID AFTER this side's bring_up
	 * snapshot, producing a stable-but-dead session (our ingress on the
	 * old id, peer transmitting on the new one). Static per-link ids
	 * make every re-negotiation converge trivially; prevention of the
	 * wedge lives in the READY gate and the quiesce-before-disable
	 * order above.
	 */

	/*
	 * Delivery rule: a session that was UP gets its bracketing
	 * link_down as always; additionally, a TERMINAL teardown (CLOSED /
	 * UNPLUG / DEAD_HW) owes the client a link_down whenever it still
	 * holds an active record (->up_delivered), even if the session was
	 * already down non-terminally -- a destroy right after a peer's BYE
	 * (LOGOUT) used to skip the upcall entirely, so the client never
	 * unpublished and every peer reboot leaked a stale usb4_rdmaN
	 * shadowing the live device. Non-terminal downs do not clear the
	 * flag: the client's record (and device) outlives session bounces.
	 */
	terminal = reason == TBFRAME_DOWN_CLOSED ||
		   reason == TBFRAME_DOWN_UNPLUG ||
		   reason == TBFRAME_DOWN_DEAD_HW;
	spin_lock_irqsave(&link->lock, flags);
	deliver_down = was_up || (link->up_delivered && terminal);
	if (terminal)
		link->up_delivered = false;
	spin_unlock_irqrestore(&link->lock, flags);
	if (deliver_down) {
		pr_info("%s: link down (%d)\n", link->name, reason);
		down_read(&tf->client_rwsem);
		if (tf->client_ops && tf->client_ops->link_down)
			tf->client_ops->link_down(tf->client_ctx, link, reason);
		up_read(&tf->client_rwsem);
	}

	/*
	 * Automatic re-handshake for every reason but the terminal ones.
	 * After a LOGOUT (peer's BYE) the peer is mid-teardown for up to its
	 * whole BYE budget: re-handshaking into that window produces the
	 * one-sided bring_up above. Wait it out; if the peer comes back
	 * sooner its fresh HELLO kicks this session immediately anyway.
	 */
	if (reason != TBFRAME_DOWN_CLOSED && reason != TBFRAME_DOWN_DEAD_HW &&
	    reason != TBFRAME_DOWN_UNPLUG)
		tbframe_link_kick(link, msecs_to_jiffies(
			reason == TBFRAME_DOWN_LOGOUT ?
			TBFRAME_BYE_SETTLE_MS : TBFRAME_RETRY_DELAY_MS));
}

static void __tbframe_link_session_step(struct tbframe_link *link)
{
	unsigned long flags;
	bool hello_done, rings_up, ready_sent, down;
	enum tbframe_down_reason reason;
	int ret;

	lockdep_assert_held(&link->session_lock);

	spin_lock_irqsave(&link->lock, flags);
	/*
	 * Entry gate, not just a queue gate. tbframe_link_kick_locked()
	 * refuses to re-arm a removing/parked/DEAD link, but a work item
	 * already queued before the gate closed still runs, and the step is
	 * also reachable directly. Re-checking here keeps the invariant
	 * local: a parked session never advances, so the client's last
	 * link_down() cannot be followed by a link_up().
	 */
	if (link->removing || link->parked ||
	    link->state == TBFRAME_STATE_DEAD) {
		spin_unlock_irqrestore(&link->lock, flags);
		return;
	}
	down = link->needs_down;
	reason = link->down_reason;
	hello_done = link->hello_done;
	rings_up = link->rings_up;
	ready_sent = link->hs.request_sent;
	spin_unlock_irqrestore(&link->lock, flags);

	if (down) {
		tbframe_link_down_session(link, reason);
		return;
	}

	if (!hello_done) {
		unsigned int attempts;

		ret = tbframe_link_hello_once(link);
		if (ret) {
			/*
			 * Under the lock: the inbound HELLO handler re-arms
			 * this budget from the dispatch context.
			 */
			spin_lock_irqsave(&link->lock, flags);
			attempts = ++link->hello_attempts;
			if (attempts >= TBFRAME_HELLO_RETRIES)
				link->hello_attempts = 0;
			spin_unlock_irqrestore(&link->lock, flags);
			if (attempts >= TBFRAME_HELLO_RETRIES) {
				pr_warn_ratelimited("%s: HELLO unanswered after %u attempts (%d); re-announcing services and retrying\n",
						    link->name, attempts, ret);
				tbframe_link_reannounce(link);
			}
			tbframe_link_kick(link,
					  msecs_to_jiffies(TBFRAME_RETRY_DELAY_MS));
			return;
		}
		rings_up = false;
	}

	if (!rings_up) {
		spin_lock_irqsave(&link->lock, flags);
		rings_up = link->rings_up;
		spin_unlock_irqrestore(&link->lock, flags);
		if (!rings_up) {
			ret = tbframe_link_bring_up(link);
			if (ret) {
				tbframe_link_kick(link,
						  msecs_to_jiffies(TBFRAME_RETRY_DELAY_MS));
				return;
			}
		}
	}

	if (!ready_sent) {
		spin_lock_irqsave(&link->lock, flags);
		ready_sent = link->hs.request_sent;
		spin_unlock_irqrestore(&link->lock, flags);
	}
	if (!ready_sent) {
		unsigned int attempts;

		ret = tbframe_link_ready_once(link);
		if (ret) {
			spin_lock_irqsave(&link->lock, flags);
			attempts = ++link->hs.attempts;
			if (attempts >= TBFRAME_READY_RETRIES)
				tb_xdomain_handshake_reset(&link->hs);
			spin_unlock_irqrestore(&link->lock, flags);
			if (attempts >= TBFRAME_READY_RETRIES) {
				/*
				 * Like HELLO, READY must never give up for
				 * good: a coordinated fleet reload exhausts
				 * any finite budget while the peer settles.
				 * The handshake was re-armed above; re-announce.
				 */
				pr_warn_ratelimited("%s: READY unanswered after %u attempts (%d); re-announcing services and retrying\n",
						    link->name, attempts, ret);
				tbframe_link_reannounce(link);
			}
			tbframe_link_kick(link,
					  msecs_to_jiffies(TBFRAME_RETRY_DELAY_MS));
			return;
		}
	}

	tbframe_link_maybe_up(link);
}

void tbframe_link_session_step(struct tbframe_link *link)
{
	mutex_lock(&link->session_lock);
	__tbframe_link_session_step(link);
	mutex_unlock(&link->session_lock);
}

static void tbframe_session_workfn(struct work_struct *work)
{
	struct tbframe_link *link = container_of(to_delayed_work(work),
						 struct tbframe_link,
						 session_work);

	tbframe_link_session_step(link);
}

/*
 * Level-triggered session verify (5 s default): read the routers' hop
 * entries back and, on a zombie (established handshake over dead paths),
 * tear down and re-handshake. Hardware hot-events are hints, never the
 * mechanism. Also carries the keepalive probe when both ends negotiated it.
 */
void tbframe_link_verify_step(struct tbframe_link *link)
{
	struct tbframe *tf = link->tf;
	unsigned long flags;
	u16 remote_hopid;
	u32 remote_caps;
	bool zombie;
	int ret;

	spin_lock_irqsave(&link->lock, flags);
	if (link->removing || link->parked ||
	    link->state != TBFRAME_STATE_UP) {
		spin_unlock_irqrestore(&link->lock, flags);
		return;
	}
	remote_hopid = link->remote_hopid;
	remote_caps = link->remote_caps;
	spin_unlock_irqrestore(&link->lock, flags);

	/* <0 = state unknown; treat as alive, never as dead. */
	ret = link->ops->paths_active(link->hw, link->local_hopid,
				      remote_hopid);
	spin_lock_irqsave(&link->lock, flags);
	zombie = tb_xdomain_session_zombie(&link->hs, ret != 0);
	if (zombie && link->state == TBFRAME_STATE_UP && !link->needs_down) {
		link->needs_down = true;
		link->down_reason = TBFRAME_DOWN_VERIFY;
		tbframe_link_kick_locked(link, 0);
	}
	spin_unlock_irqrestore(&link->lock, flags);
	if (zombie)
		return;

	if (tf->keepalive && (remote_caps & TBFRAME_WIRE_CAP_KEEPALIVE)) {
		struct tbframe_frame *f;

		/* Charged to the control reserve; skip a tick when full. */
		if (!tbframe_alloc_frame(link, TBFRAME_KEEPALIVE_LEN, true,
					 &f)) {
			f->pdf = TBFRAME_PDF_KEEPALIVE;
			tbframe_wire_put_le64(f->data, link->local_cookie);
			if (tbframe_xmit(link, f))
				tbframe_frame_free(link, f);
		}
	}

	spin_lock_irqsave(&link->lock, flags);
	if (!link->removing && link->state == TBFRAME_STATE_UP)
		mod_timer(&link->verify_timer,
			  jiffies + msecs_to_jiffies(tf->verify_ms));
	spin_unlock_irqrestore(&link->lock, flags);
}

static void tbframe_verify_workfn(struct work_struct *work)
{
	struct tbframe_link *link = container_of(work, struct tbframe_link,
						 verify_work);

	tbframe_link_verify_step(link);
}

static void tbframe_verify_timer_fn(struct timer_list *t)
{
	struct tbframe_link *link = timer_container_of(link, t, verify_timer);
	unsigned long flags;

	/*
	 * Re-queue gate, same discipline as tbframe_link_kick_locked(). The
	 * timer_shutdown_sync() + cancel_work_sync() pair in link_destroy is
	 * what actually guarantees quiescence (Documentation: the
	 * timer-schedules-work-rearms-timer pattern in
	 * kernel/time/timer.c:timer_shutdown_sync); this only keeps the
	 * window small.
	 */
	spin_lock_irqsave(&link->lock, flags);
	if (!link->removing && !link->parked)
		queue_work(link->tf->wq, &link->verify_work);
	spin_unlock_irqrestore(&link->lock, flags);
}

/* Inbound control-plane packet for one link. Returns 1 when consumed. */
int tbframe_link_handle_packet(struct tbframe_link *link, const void *buf,
			       size_t size)
{
	struct tbframe_wire_hello local;
	struct tbframe_wire_hello remote;
	struct tbframe_wire_info info;
	u8 reply[TBFRAME_WIRE_HELLO_MSG_SIZE];
	unsigned long flags;
	int ret;

	ret = tbframe_wire_parse_hello(buf, size, &remote, &info);
	if (ret) {
		int ver = tbframe_wire_peek_version(buf, size);

		if (ver >= 0 && ver != TBFRAME_WIRE_VERSION)
			pr_warn_ratelimited("peer speaks tbframe wire v%d, this module speaks v%u; update the fleet\n",
					    ver, TBFRAME_WIRE_VERSION);
		return 0;
	}
	if (info.route != link->route)
		return 0;

	/*
	 * Inbound-request vs teardown fence. A peer that is mid-handshake
	 * while this side unbinds its service (or unloads) keeps HELLOing;
	 * answering from a link whose rings/HopIDs/paths are being undone
	 * mutates session state behind the teardown's back. Refuse and let
	 * the peer retry -- convergence costs one retry interval, and the
	 * teardown becomes provably free of inbound mutation.
	 *
	 * Returning 0 (not consumed) is deliberate: another link may still
	 * want this packet, and an unconsumed request makes the core answer
	 * the peer with an error rather than silence.
	 */
	spin_lock_irqsave(&link->lock, flags);
	ret = link->removing || link->state == TBFRAME_STATE_DEAD;
	/*
	 * A parked or mid-teardown session must not negotiate either:
	 * answering HELLO/READY from it lets the peer run a one-sided
	 * bring_up against a session that is being dismantled, leaving its
	 * freshly-programmed path endpoint unmatched for seconds (v3 storm
	 * cycle 1: the receiver's post-BYE rebuild HELLOed the parked
	 * sender's dispatch, sat one-sided through the sender's whole BYE
	 * budget, and the eventually-paired session was born dead). Refuse;
	 * the peer's retry lands after this side settles. BYE and the ack
	 * ops stay handled: teardown is exactly when they matter.
	 */
	if (!ret && (link->parked || link->needs_down) &&
	    (info.op == TBFRAME_WIRE_OP_HELLO ||
	     info.op == TBFRAME_WIRE_OP_READY))
		ret = 1;
	spin_unlock_irqrestore(&link->lock, flags);
	if (ret)
		return 0;

	switch (info.op) {
	case TBFRAME_WIRE_OP_HELLO_ACK:
		/*
		 * Observed before tb_xdomain_request() response matching;
		 * apply but do not consume, or the requester times out.
		 */
		if (remote.proto_version == TBFRAME_WIRE_VERSION) {
			spin_lock_irqsave(&link->lock, flags);
			tbframe_link_apply_remote_locked(link, &remote);
			link->hello_done = true;
			spin_unlock_irqrestore(&link->lock, flags);
		}
		return 0;

	case TBFRAME_WIRE_OP_READY_ACK:
		return 0;

	case TBFRAME_WIRE_OP_HELLO:
		if (remote.proto_version != TBFRAME_WIRE_VERSION) {
			pr_warn_ratelimited("%s: peer HELLO v%u refused (local v%u)\n",
					    link->name, remote.proto_version,
					    TBFRAME_WIRE_VERSION);
			return 1;
		}
		spin_lock_irqsave(&link->lock, flags);
		tbframe_link_apply_remote_locked(link, &remote);
		/*
		 * An inbound HELLO while established means the peer restarted
		 * without a link edge; our session points at its freed rings.
		 * Supersede (shared cross-driver contract) and let the session
		 * work run the full down/re-handshake.
		 */
		if (link->state == TBFRAME_STATE_UP && !link->needs_down) {
			tb_xdomain_handshake_supersede(&link->hs);
			link->needs_down = true;
			link->down_reason = TBFRAME_DOWN_SUPERSEDE;
		}
		link->hello_done = true;
		/* Peer is actively negotiating: re-arm our retry budgets. */
		link->hello_attempts = 0;
		link->hs.attempts = 0;
		tbframe_link_kick_locked(link, 0);
		spin_unlock_irqrestore(&link->lock, flags);

		tbframe_link_fill_local_hello(link, &local);
		ret = tbframe_wire_build_hello(reply, sizeof(reply), &local,
					       TBFRAME_WIRE_OP_HELLO_ACK,
					       info.seq, link->route,
					       info.xdomain_sequence);
		if (ret >= 0)
			ret = link->ops->control_response(link->hw, reply,
							  sizeof(reply));
		if (ret < 0)
			pr_warn("%s: HELLO_ACK failed: %d\n", link->name, ret);
		return 1;

	case TBFRAME_WIRE_OP_READY: {
		bool enabled;

		spin_lock_irqsave(&link->lock, flags);
		link->hs.peer_seen = true;
		/*
		 * A pending down (supersede queued by the HELLO that preceded
		 * this READY) means ->paths_enabled still describes the OLD
		 * session's hop entries, which the session work is about to
		 * disable. Acking from them certifies paths that are already
		 * scheduled for teardown: the peer goes UP and streams a full
		 * TX ring into a hop entry mid-removal, which is the observed
		 * router-level egress wedge (TX ring enabled, everything
		 * posted, nothing consumed, reboot-only recovery).
		 */
		enabled = link->paths_enabled && !link->needs_down;
		tbframe_link_kick_locked(link, 0);
		spin_unlock_irqrestore(&link->lock, flags);

		/*
		 * Withhold the ack until our own paths are enabled; the peer
		 * keeps retrying READY and the ack then certifies mutual
		 * readiness (see tbframe_link_ready_once()).
		 */
		if (!enabled)
			return 1;

		tbframe_link_fill_local_hello(link, &local);
		ret = tbframe_wire_build_hello(reply, sizeof(reply), &local,
					       TBFRAME_WIRE_OP_READY_ACK,
					       info.seq, link->route,
					       info.xdomain_sequence);
		if (ret >= 0)
			ret = link->ops->control_response(link->hw, reply,
							  sizeof(reply));
		if (ret < 0)
			pr_warn("%s: READY_ACK failed: %d\n", link->name, ret);
		return 1;
	}
	case TBFRAME_WIRE_OP_BYE: {
		bool quiesced;

		spin_lock_irqsave(&link->lock, flags);
		if (link->state == TBFRAME_STATE_UP && !link->needs_down) {
			link->needs_down = true;
			link->down_reason = TBFRAME_DOWN_LOGOUT;
			tbframe_link_kick_locked(link, 0);
		}
		/*
		 * The ack certifies "no more frames from this side": withhold
		 * it until the session has left UP and its rings are being
		 * torn down; the peer retries BYE (bounded) and the retry
		 * lands after our down has run.
		 */
		quiesced = link->state != TBFRAME_STATE_UP && link->tx_quiesced;
		spin_unlock_irqrestore(&link->lock, flags);
		if (!quiesced)
			return 1;

		tbframe_link_fill_local_hello(link, &local);
		ret = tbframe_wire_build_hello(reply, sizeof(reply), &local,
					       TBFRAME_WIRE_OP_BYE_ACK,
					       info.seq, link->route,
					       info.xdomain_sequence);
		if (ret >= 0)
			ret = link->ops->control_response(link->hw, reply,
							  sizeof(reply));
		if (ret < 0)
			pr_warn("%s: BYE_ACK failed: %d\n", link->name, ret);
		return 1;
	}

	case TBFRAME_WIRE_OP_BYE_ACK:
		/* Response matching in the requester consumes it. */
		return 0;

	default:
		return 0;
	}
}

/*
 * Look up the next candidate link past @skip and take a reference to it.
 * Returns NULL when the walk is exhausted. tf->lock held only here.
 */
static struct tbframe_link *tbframe_dispatch_next(struct tbframe *tf,
						  const void *token,
						  unsigned int *skip)
{
	struct tbframe_link *link, *target = NULL;
	unsigned int idx = 0;

	mutex_lock(&tf->lock);
	list_for_each_entry(link, &tf->links, node) {
		if (idx++ < *skip)
			continue;
		if (token && link->ops->match &&
		    !link->ops->match(link->hw, token))
			continue;
		/*
		 * A link still on the list still holds its base reference:
		 * tbframe_link_destroy() delists under tf->lock before it
		 * drops that reference, so this get cannot resurrect a
		 * dying link, and destroy's bounded refs-zero wait covers
		 * the reference taken here.
		 */
		refcount_inc(&link->refcnt);
		target = link;
		*skip = idx;
		break;
	}
	mutex_unlock(&tf->lock);
	return target;
}

/*
 * Inbound control-plane dispatch. Runs from the thunderbolt core's XDomain
 * dispatch walk, which is itself the ctl RX ring's callback context and
 * holds the core's global xdomain_dispatch_lock across this call
 * (drivers/thunderbolt/xdomain.c, commit 054b92c).
 *
 * Consequently this function must be SHORT and must never block on anything
 * that can wait for the control channel: tf->lock is dropped before the
 * per-link handler runs. Holding it across the handler used to close a
 * three-way cycle -- dispatch blocked on tf->lock, tbframe_unregister_client()
 * holding tf->lock while waiting for session_lock, and the session work
 * holding session_lock inside tb_xdomain_request() waiting for a control
 * response that only this very ctl RX worker can deliver. It resolved only
 * on the 1 s control timeout, stalling every XDomain protocol on the host
 * (including the core's own handshakes) for the duration, once per retry.
 *
 * The index cursor is re-evaluated under tf->lock on every step. A link
 * removed mid-walk can shift the indices and cost one skipped delivery;
 * the peer retries, and no reference or lock is held across the gap.
 */
int tbframe_handle_packet(struct tbframe *tf, const void *token,
			  const void *buf, size_t size)
{
	unsigned int skip = 0;
	int ret = 0;

	for (;;) {
		struct tbframe_link *link;

		link = tbframe_dispatch_next(tf, token, &skip);
		if (!link)
			return ret;

		ret = tbframe_link_handle_packet(link, buf, size);
		tbframe_link_put(link);
		if (ret)
			return ret;
	}
}

/* Uncharge and return a TX frame to the pool; lock held. Returns whether
 * the admission window reopened for a blocked producer. */
static bool tbframe_tx_return_locked(struct tbframe_link *link,
				     struct tbframe_frame_priv *f)
{
	lockdep_assert_held(&link->lock);
	if (f->charged_ctrl) {
		if (!WARN_ON(!link->ctrl_inflight))
			link->ctrl_inflight--;
		f->charged_ctrl = false;
	}
	if (f->charged_data) {
		if (!WARN_ON(!link->data_inflight))
			link->data_inflight--;
		f->charged_data = false;
	}
	list_add_tail(&f->node, &link->tx_free);
	if (link->tx_blocked && link->state == TBFRAME_STATE_UP &&
	    !link->removing) {
		link->tx_blocked = false;
		return true;
	}
	return false;
}

void tbframe_core_tx_complete(struct tbframe_frame_priv *f, bool canceled)
{
	struct tbframe_link *link = f->link;
	unsigned long flags;
	bool release;

	spin_lock_irqsave(&link->lock, flags);
	release = tbframe_tx_return_locked(link, f) && !canceled;
	spin_unlock_irqrestore(&link->lock, flags);
	if (release)
		queue_work(link->tf->wq, &link->tx_released_work);
	tbframe_link_put(link);
}

static void tbframe_tx_released_workfn(struct work_struct *work)
{
	struct tbframe_link *link = container_of(work, struct tbframe_link,
						 tx_released_work);
	struct tbframe *tf = link->tf;

	down_read(&tf->client_rwsem);
	if (tf->client_ops && tf->client_ops->tx_released)
		tf->client_ops->tx_released(tf->client_ctx, link);
	up_read(&tf->client_rwsem);
}

void tbframe_core_rx_complete(struct tbframe_frame_priv *f, bool canceled,
			      u16 len, u8 pdf, bool bad)
{
	struct tbframe_link *link = f->link;
	struct tbframe *tf = link->tf;
	unsigned long flags;
	bool up;

	if (canceled) {
		tbframe_frame_putback_rx(f);
		return;
	}

	spin_lock_irqsave(&link->lock, flags);
	up = link->state == TBFRAME_STATE_UP;
	spin_unlock_irqrestore(&link->lock, flags);

	/* Loss model: CRC drops and post-link_down frames never reach rx(). */
	if (!up || bad || len > TBFRAME_MAX_FRAME) {
		tbframe_frame_recycle_rx(f);
		return;
	}

	if (pdf == TBFRAME_PDF_KEEPALIVE) {
		if (len >= TBFRAME_KEEPALIVE_LEN) {
			u64 cookie = tbframe_wire_get_le64(f->frame.data);

			spin_lock_irqsave(&link->lock, flags);
			/*
			 * Cookie mismatch: the peer rebooted inside one
			 * verify interval. Supersede signal per spec §3.
			 */
			if (cookie != link->remote_cookie &&
			    link->state == TBFRAME_STATE_UP &&
			    !link->needs_down) {
				link->needs_down = true;
				link->down_reason = TBFRAME_DOWN_SUPERSEDE;
				tbframe_link_kick_locked(link, 0);
			}
			spin_unlock_irqrestore(&link->lock, flags);
		}
		tbframe_frame_recycle_rx(f);
		return;
	}

	f->frame.len = len;
	f->frame.pdf = pdf;
	refcount_set(&f->rx_refs, 1);
	down_read(&tf->client_rwsem);
	if (tf->client_ops && tf->client_ops->rx)
		tf->client_ops->rx(tf->client_ctx, link, &f->frame);
	up_read(&tf->client_rwsem);
	if (refcount_dec_and_test(&f->rx_refs))
		tbframe_frame_recycle_rx(f);
}

/* Public client API. */

int tbframe_alloc_frame(struct tbframe_link *link, u16 len, bool is_ctrl,
			struct tbframe_frame **frame)
{
	struct tbframe_frame_priv *f;
	unsigned long flags;

	if (!link || !frame || !len || len > TBFRAME_MAX_FRAME)
		return -EINVAL;

	spin_lock_irqsave(&link->lock, flags);
	if (link->state != TBFRAME_STATE_UP) {
		spin_unlock_irqrestore(&link->lock, flags);
		return -ENETDOWN;
	}
	if (is_ctrl ? link->ctrl_inflight >= TBFRAME_CTRL_RESERVE :
		      link->data_inflight >= link->data_window)
		goto nospc;
	f = list_first_entry_or_null(&link->tx_free, struct tbframe_frame_priv,
				     node);
	if (!f)
		goto nospc;
	list_del_init(&f->node);
	if (is_ctrl) {
		link->ctrl_inflight++;
		f->charged_ctrl = true;
	} else {
		link->data_inflight++;
		f->charged_data = true;
	}
	f->frame.len = len;
	f->frame.pdf = TBFRAME_PDF_DATA;
	f->frame.is_ctrl = is_ctrl;
	refcount_inc(&link->refcnt);
	spin_unlock_irqrestore(&link->lock, flags);
	*frame = &f->frame;
	return 0;

nospc:
	link->tx_blocked = true;
	spin_unlock_irqrestore(&link->lock, flags);
	return -ENOSPC;
}
EXPORT_SYMBOL_GPL(tbframe_alloc_frame);

int tbframe_xmit(struct tbframe_link *link, struct tbframe_frame *frame)
{
	struct tbframe_frame_priv *f;
	unsigned long flags;
	bool ok;
	int ret;

	if (!link || !frame)
		return -EINVAL;
	f = container_of(frame, struct tbframe_frame_priv, frame);
	if (f->link != link || f->is_rx)
		return -EINVAL;

	/*
	 * hw_active brackets the state-check-to-ring_tx window so
	 * down-session can drain publishers before touching the rings.
	 */
	atomic_inc(&link->hw_active);
	spin_lock_irqsave(&link->lock, flags);
	ok = link->state == TBFRAME_STATE_UP && link->rings_up;
	spin_unlock_irqrestore(&link->lock, flags);
	ret = ok ? link->ops->ring_tx(link->hw, f) : -ENETDOWN;
	atomic_dec(&link->hw_active);
	return ret;
}
EXPORT_SYMBOL_GPL(tbframe_xmit);

void tbframe_frame_free(struct tbframe_link *link, struct tbframe_frame *frame)
{
	struct tbframe_frame_priv *f;
	unsigned long flags;
	bool release;

	if (!link || !frame)
		return;
	f = container_of(frame, struct tbframe_frame_priv, frame);
	if (WARN_ON(f->link != link || f->is_rx))
		return;

	spin_lock_irqsave(&link->lock, flags);
	release = tbframe_tx_return_locked(link, f);
	spin_unlock_irqrestore(&link->lock, flags);
	if (release)
		queue_work(link->tf->wq, &link->tx_released_work);
	tbframe_link_put(link);
}
EXPORT_SYMBOL_GPL(tbframe_frame_free);

void tbframe_frame_get_rx(struct tbframe_frame *frame)
{
	struct tbframe_frame_priv *f = container_of(frame,
						    struct tbframe_frame_priv,
						    frame);

	refcount_inc(&f->rx_refs);
}
EXPORT_SYMBOL_GPL(tbframe_frame_get_rx);

void tbframe_frame_put_rx(struct tbframe_link *link,
			  struct tbframe_frame *frame)
{
	struct tbframe_frame_priv *f = container_of(frame,
						    struct tbframe_frame_priv,
						    frame);

	if (WARN_ON(!link || f->link != link || !f->is_rx))
		return;
	if (refcount_dec_and_test(&f->rx_refs))
		tbframe_frame_recycle_rx(f);
}
EXPORT_SYMBOL_GPL(tbframe_frame_put_rx);

const char *tbframe_link_name(const struct tbframe_link *link)
{
	return link ? link->name : "<null>";
}
EXPORT_SYMBOL_GPL(tbframe_link_name);

void tbframe_link_info(const struct tbframe_link *link,
		       struct tbframe_link_info *info)
{
	struct tbframe_link *l = (struct tbframe_link *)link;
	unsigned long flags;

	if (!link || !info)
		return;
	spin_lock_irqsave(&l->lock, flags);
	tbframe_link_fill_info_locked(l, info);
	spin_unlock_irqrestore(&l->lock, flags);
	if (l->ops->link_attrs)
		l->ops->link_attrs(l->hw, &info->width, &info->speed);
	if (l->ops->peer_identity)
		l->ops->peer_identity(l->hw, info->remote_uuid,
				      info->remote_name,
				      sizeof(info->remote_name));
}
EXPORT_SYMBOL_GPL(tbframe_link_info);

/* Link lifecycle. */

static void tbframe_link_free_pools(struct tbframe_link *link)
{
	u16 i;

	for (i = 0; link->tx_frames && i < link->tx_frame_count; i++) {
		if (link->ops->unmap_frame && link->tx_frames[i].frame.data)
			link->ops->unmap_frame(link->hw, &link->tx_frames[i],
					       true);
		kfree(link->tx_frames[i].frame.data);
	}
	for (i = 0; link->rx_frames && i < link->rx_frame_count; i++) {
		if (link->ops->unmap_frame && link->rx_frames[i].frame.data)
			link->ops->unmap_frame(link->hw, &link->rx_frames[i],
					       false);
		kfree(link->rx_frames[i].frame.data);
	}
	kfree(link->tx_frames);
	kfree(link->rx_frames);
	link->tx_frames = NULL;
	link->rx_frames = NULL;
}

static int tbframe_link_alloc_pool(struct tbframe_link *link, bool tx)
{
	struct tbframe_frame_priv *frames;
	struct list_head *free_list = tx ? &link->tx_free : &link->rx_free;
	u16 count = link->tf->ring_entries;
	u16 i;
	int ret;

	frames = kcalloc(count, sizeof(*frames), GFP_KERNEL);
	if (!frames)
		return -ENOMEM;

	for (i = 0; i < count; i++) {
		struct tbframe_frame_priv *f = &frames[i];

		f->frame.data = kzalloc(TBFRAME_MAX_FRAME, GFP_KERNEL);
		if (!f->frame.data)
			goto err;
		f->link = link;
		f->is_rx = !tx;
		INIT_LIST_HEAD(&f->node);
		if (link->ops->map_frame) {
			ret = link->ops->map_frame(link->hw, f, tx);
			if (ret) {
				kfree(f->frame.data);
				f->frame.data = NULL;
				goto err;
			}
		}
		list_add_tail(&f->node, free_list);
	}

	if (tx) {
		link->tx_frames = frames;
		link->tx_frame_count = count;
	} else {
		link->rx_frames = frames;
		link->rx_frame_count = count;
	}
	return 0;

err:
	while (i--) {
		if (link->ops->unmap_frame)
			link->ops->unmap_frame(link->hw, &frames[i], tx);
		kfree(frames[i].frame.data);
	}
	kfree(frames);
	INIT_LIST_HEAD(free_list);
	return -ENOMEM;
}

struct tbframe_link *tbframe_link_create(struct tbframe *tf,
					 const struct tbframe_hw_ops *ops,
					 void *hw, u64 route, u64 gid_eui64,
					 bool autostart)
{
	struct tbframe_link *link;
	int ret;

	link = kzalloc(sizeof(*link), GFP_KERNEL);
	if (!link)
		return ERR_PTR(-ENOMEM);

	link->tf = tf;
	link->ops = ops;
	link->hw = hw;
	link->route = route;
	link->local_gid_eui64 = gid_eui64;
	link->local_cookie = get_random_u64();
	snprintf(link->name, sizeof(link->name), "tbframe0x%llx", route);

	mutex_init(&link->session_lock);
	spin_lock_init(&link->lock);
	INIT_LIST_HEAD(&link->node);
	INIT_LIST_HEAD(&link->tx_free);
	INIT_LIST_HEAD(&link->rx_free);
	INIT_DELAYED_WORK(&link->session_work, tbframe_session_workfn);
	INIT_WORK(&link->tx_released_work, tbframe_tx_released_workfn);
	INIT_WORK(&link->verify_work, tbframe_verify_workfn);
	timer_setup(&link->verify_timer, tbframe_verify_timer_fn, 0);
	refcount_set(&link->refcnt, 1);
	init_completion(&link->refs_zero);
	atomic_set(&link->hw_active, 0);
	link->state = TBFRAME_STATE_INIT;
	link->tx_quiesced = true;
	tb_xdomain_handshake_reset(&link->hs);

	ret = ops->alloc_out_hopid(hw);
	if (ret < 0)
		goto err_free;
	link->local_hopid = ret;

	ret = tbframe_link_alloc_pool(link, true);
	if (ret)
		goto err_hopid;
	ret = tbframe_link_alloc_pool(link, false);
	if (ret)
		goto err_pools;

	mutex_lock(&tf->lock);
	list_add_tail(&link->node, &tf->links);
	mutex_unlock(&tf->lock);

	pr_info("%s: link created local_hopid=%d\n", link->name,
		link->local_hopid);
	if (autostart)
		tbframe_link_kick(link, 0);
	return link;

err_pools:
	tbframe_link_free_pools(link);
err_hopid:
	ops->release_out_hopid(hw, link->local_hopid);
err_free:
	kfree(link);
	return ERR_PTR(ret);
}

/*
 * Bounded wait for every client-held frame ref to drain; the rings were
 * already fenced, so this normally completes on the first tick. Copies the
 * legacy rail-teardown discipline: warn each interval, force at the cap.
 */
static bool tbframe_link_wait_refs_zero(struct tbframe_link *link)
{
	struct tbframe *tf = link->tf;
	unsigned int warn = tf->teardown_warn_ms ? tf->teardown_warn_ms : 2000;
	unsigned int cap = tf->teardown_force_ms;
	unsigned int waited = 0;

	for (;;) {
		if (wait_for_completion_timeout(&link->refs_zero,
						msecs_to_jiffies(warn)))
			return true;

		waited += warn;
		pr_err("%s: teardown waiting for frame refs=%u (%ums%s)\n",
		       link->name, refcount_read(&link->refcnt), waited,
		       cap ? "" : ", no cap");
		if (cap && waited >= cap)
			return false;
	}
}

int tbframe_link_destroy(struct tbframe_link *link,
			 enum tbframe_down_reason reason)
{
	struct tbframe *tf = link->tf;
	unsigned long flags;

	spin_lock_irqsave(&link->lock, flags);
	link->removing = true;
	spin_unlock_irqrestore(&link->lock, flags);

	/* Unlink first: tf->lock is always taken outside session_lock. */
	mutex_lock(&tf->lock);
	list_del_init(&link->node);
	mutex_unlock(&tf->lock);

	timer_shutdown_sync(&link->verify_timer);
	cancel_delayed_work_sync(&link->session_work);
	cancel_work_sync(&link->verify_work);

	mutex_lock(&link->session_lock);
	tbframe_link_down_session(link, reason);
	mutex_unlock(&link->session_lock);

	cancel_work_sync(&link->tx_released_work);

	link->ops->release_out_hopid(link->hw, link->local_hopid);

	/*
	 * Bounded refs wait, then a deliberate leak: a still-held ref means
	 * the client (or dead hardware that never cancelled a frame) still
	 * points into the pool; freeing would be a UAF, and wedging an
	 * unload forever is worse than leaking. Poison the link so nothing
	 * ever re-arms it.
	 */
	tbframe_link_put(link);
	if (!tbframe_link_wait_refs_zero(link)) {
		spin_lock_irqsave(&link->lock, flags);
		link->state = TBFRAME_STATE_DEAD;
		spin_unlock_irqrestore(&link->lock, flags);
		pr_err("%s: teardown FORCED refs=%u: rings fenced, leaking link to avoid UAF\n",
		       link->name, refcount_read(&link->refcnt));
		return -EBUSY;
	}

	tbframe_link_free_pools(link);
	pr_info("%s: link destroyed\n", link->name);
	kfree(link);
	return 0;
}

/* Client registration (one client per module instance). */

int tbframe_register_client_tf(struct tbframe *tf,
			       const struct tbframe_client_ops *ops, void *ctx)
{
	struct tbframe_link *link;
	unsigned long flags;

	if (!ops)
		return -EINVAL;
	if (!tf)
		return -ENODEV;

	down_write(&tf->client_rwsem);
	if (tf->client_ops) {
		up_write(&tf->client_rwsem);
		return -EBUSY;
	}
	tf->client_ops = ops;
	tf->client_ctx = ctx;
	up_write(&tf->client_rwsem);

	/* Unpark and kick every session; established links replay link_up. */
	mutex_lock(&tf->lock);
	list_for_each_entry(link, &tf->links, node) {
		spin_lock_irqsave(&link->lock, flags);
		link->parked = false;
		if (link->state == TBFRAME_STATE_UP)
			link->announce_pending = true;
		tbframe_link_kick_locked(link, 0);
		spin_unlock_irqrestore(&link->lock, flags);
	}
	mutex_unlock(&tf->lock);
	return 0;
}

int tbframe_register_client(const struct tbframe_client_ops *ops, void *ctx)
{
	return tbframe_register_client_tf(tbframe_instance(), ops, ctx);
}
EXPORT_SYMBOL_GPL(tbframe_register_client);

/*
 * Close one link for a departing client. Called with a reference held and
 * NO tf->lock: down_session() runs a bounded publisher drain and a ring
 * stop (tb_ring_stop() flushes the ring's work item), which must never be
 * done under the lock the inbound dispatch path needs.
 */
static void tbframe_link_park(struct tbframe_link *link)
{
	unsigned long flags;

	/*
	 * Park BEFORE closing. Between down_session() and the client_rwsem
	 * fence below, an inbound HELLO/READY can still mark the handshake
	 * and kick the session work, which would drive the link back UP and
	 * deliver a link_up() to a client that has already seen its final
	 * link_down() -- and then never see the matching link_down again.
	 * Parked, every re-queue gate refuses, so link_down is delivered
	 * exactly once per up link and is the last upcall.
	 */
	spin_lock_irqsave(&link->lock, flags);
	link->parked = true;
	spin_unlock_irqrestore(&link->lock, flags);

	cancel_delayed_work_sync(&link->session_work);

	mutex_lock(&link->session_lock);
	tbframe_link_down_session(link, TBFRAME_DOWN_CLOSED);
	mutex_unlock(&link->session_lock);
}

void tbframe_unregister_client_tf(struct tbframe *tf)
{
	unsigned int skip = 0;

	if (!tf)
		return;

	/*
	 * Close every link for the departing client: sessions drop to the
	 * negotiation floor and are parked (CLOSED does not re-handshake);
	 * a later register unparks and kicks them again. One link at a time,
	 * referenced, with tf->lock dropped across the teardown.
	 */
	for (;;) {
		struct tbframe_link *link;

		link = tbframe_dispatch_next(tf, NULL, &skip);
		if (!link)
			break;
		tbframe_link_park(link);
		tbframe_link_put(link);
	}

	/* Fence: returns only when no upcall is running or can run again. */
	down_write(&tf->client_rwsem);
	tf->client_ops = NULL;
	tf->client_ctx = NULL;
	up_write(&tf->client_rwsem);
}

void tbframe_unregister_client(void)
{
	tbframe_unregister_client_tf(tbframe_instance());
}
EXPORT_SYMBOL_GPL(tbframe_unregister_client);
