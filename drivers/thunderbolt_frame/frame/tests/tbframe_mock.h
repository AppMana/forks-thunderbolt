/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Mock ring layer + client recorder for the tbframe KUnit contract tests.
 * Implements every tbframe_hw_ops hook in software: control requests are
 * answered from a configurable peer HELLO, rings are lists, and
 * stop_rings() cancels in-flight frames through the real completion
 * entry points exactly like tb_ring_stop() (unless never_cancel models
 * dead hardware). Included by each test file; everything is file-local.
 */
#ifndef TBFRAME_TESTS_MOCK_H
#define TBFRAME_TESTS_MOCK_H

#include <kunit/test.h>
#include <linux/slab.h>

#include "../tbframe_priv.h"

/*
 * Lockstep lever with main.c's data_proof module parameter: 1 = the shipped
 * driver refuses to declare a link up until a frame has crossed the data ring,
 * 0 = the pre-2.46 driver, which went up on the control plane alone. Flip to 0
 * to run the tbframe_datapath suite RED against the old behaviour.
 */
#ifndef TBFRAME_DATA_PROOF
#define TBFRAME_DATA_PROOF 1
#endif

#define TBFRAME_MOCK_MAX_REQS	16
#define TBFRAME_MOCK_MAX_EVENTS	32

struct tbframe_mock {
	/* peer identity used to answer HELLO/READY requests */
	struct tbframe_wire_hello peer;
	u64		route;
	int		control_err;	/* fail every control request */
	bool		fail_ready;	/* fail only READY requests */
	u16		response_op;	/* non-zero overrides the protocol reply op */
	/*
	 * Control-channel delay model: capture the next HELLO_ACK but report the
	 * request timed out, then return that exact old wire response to the next
	 * HELLO.  This models a response retained past its requester lifetime;
	 * production still performs all request construction and response parsing.
	 */
	bool		delay_next_hello_ack;
	bool		delayed_hello_ack_pending;
	u8		delayed_hello_ack[TBFRAME_WIRE_HELLO_MSG_SIZE];
	bool		ready_ack_u8_seq;
	bool		never_cancel;	/* dead hw: stop_rings returns nothing */
	int		paths_active_ret;
	/*
	 * Dead bulk data path with a perfectly healthy control plane: rings
	 * allocate and start, hop entries read back enabled (paths_active_ret
	 * stays 1), HELLO/READY/BYE are all answered -- and not one byte
	 * crosses the data ring. This is the measured 2026-08-23 post-reload
	 * state on 5 of 10 chain links. Default false: a healthy modelled peer
	 * answers each keepalive with one of its own, which is what a real
	 * peer's verify cadence does.
	 */
	bool		datapath_dead;
	/* Directional faults: local TX cannot reach the peer, or vice versa. */
	bool		local_tx_dead;
	bool		peer_tx_dead;
	u64		peer_keepalive_seq;
	u64		peer_ack_cookie;
	u64		peer_ack_sequence;
	u64		stale_peer_cookie;
	unsigned int	stale_peer_keepalives;
	bool		tx_consumer_stalled;
	unsigned int	data_path_failure_reports;
	unsigned int	quarantine_requests;
	bool		rings_quarantined;
	int		quarantine_local_hopid;
	int		quarantine_remote_hopid;
	unsigned int	data_proven_reports;
	/*
	 * One-shot: deliver a peer frame from inside quiesce_tx(), i.e. in
	 * the teardown window after the session state has been reset but
	 * before stop_rings() cancels the posted descriptors. Models a frame
	 * the peer had already put on the wire before it learned the session
	 * was going down, which is why it ignores @datapath_dead -- the frame
	 * predates the path dying.
	 */
	bool		deliver_on_quiesce;
	/*
	 * Leave our own keepalives sitting in tx_queue instead of completing
	 * them the way hardware would, so a test can inspect the frame.
	 */
	bool		hold_keepalives;
	unsigned int	peer_keepalives;	/* frames the model peer sent */
	unsigned int	keepalives_sent;	/* keepalives WE put on the ring */

	bool		rings_alloced;
	bool		rings_started;
	bool		rings_e2e;
	bool		paths_on;
	bool		enable_seen;
	int		enable_paths_err;
	int		disable_paths_err;
	unsigned int	enable_paths_calls;
	unsigned int	disable_paths_calls;
	unsigned int	paths_active_calls;
	bool		freed_rings_while_paths_on;
	bool		released_hopid_while_paths_on;
	/* out-HopID allocator: ida-like, lowest free, never a held id */
	int		next_out_hopid;
	unsigned int	out_hopid_allocs;
	unsigned int	out_hopid_releases;
	int		last_release_out_hopid;
	/* transmit HopID the link advertised in its most recent request */
	int		last_request_tx_hopid;
	int		in_hopid;
	int		last_alloc_in_hopid;	/* -1 when never allocated */
	int		last_release_in_hopid;	/* -1 when never released */
	unsigned int	in_hopid_allocs;
	unsigned int	in_hopid_releases;
	int		last_disable_remote_hopid;
	unsigned int	reannounce_calls;
	unsigned int	rx_posted_before_enable;

	struct list_head tx_queue;	/* ring_frame.list of queued TX */
	struct list_head rx_posted;	/* ring_frame.list of posted RX */
	unsigned int	tx_queued;
	unsigned int	rx_posted_count;

	u8		last_response[TBFRAME_WIRE_HELLO_MSG_SIZE];
	bool		have_response;
	u16		req_ops[TBFRAME_MOCK_MAX_REQS];
	u32		req_caps[TBFRAME_MOCK_MAX_REQS];
	unsigned int	req_count;
	unsigned int	bye_count;
	bool		bye_rings_started;
	unsigned int	bye_hw_calls_at;
	struct tbframe_link *must_be_parked_before_bye;
	bool		bye_started_before_all_links_parked;
	bool		bye_started_before_all_session_work_stopped;
	struct tbframe_link *must_be_quiet_before_disable;
	bool		disable_started_before_all_links_parked;
	bool		disable_started_before_all_activity_stopped;

	/* teardown call order (enum tbframe_mock_hw_call tokens) */
	u8		hw_calls[TBFRAME_MOCK_MAX_EVENTS];
	unsigned int	hw_call_count;
};

enum tbframe_mock_hw_call {
	TBFRAME_HW_QUIESCE_TX = 5,
	TBFRAME_HW_STOP_RINGS = 1,
	TBFRAME_HW_FREE_RINGS,
	TBFRAME_HW_DISABLE_PATHS,
	TBFRAME_HW_RELEASE_IN_HOPID,
};

static void tbframe_mock_hw_call(struct tbframe_mock *m, u8 call)
{
	if (m->hw_call_count < TBFRAME_MOCK_MAX_EVENTS)
		m->hw_calls[m->hw_call_count++] = call;
}

/* Position of @call's first occurrence at or after @from, or -1. */
static __maybe_unused int tbframe_mock_hw_call_pos(const struct tbframe_mock *m,
						   u8 call, unsigned int from)
{
	unsigned int i;

	for (i = from; i < m->hw_call_count; i++)
		if (m->hw_calls[i] == call)
			return i;
	return -1;
}

enum tbframe_mock_ev {
	TBFRAME_EV_UP = 1,
	TBFRAME_EV_DOWN,
	TBFRAME_EV_RX,
	TBFRAME_EV_TX_RELEASED,
};

struct tbframe_mock_client {
	spinlock_t	lock;
	u8		events[TBFRAME_MOCK_MAX_EVENTS];
	int		reasons[TBFRAME_MOCK_MAX_EVENTS];
	unsigned int	count;
	unsigned int	up_count;
	unsigned int	down_count;
	unsigned int	rx_count;
	unsigned int	tx_released_count;
	struct tbframe_link_info last_info;
	u16		last_rx_len;
	u8		last_rx_pdf;
};

static void tbframe_mock_client_event(struct tbframe_mock_client *c, u8 ev,
				      int reason)
{
	unsigned long flags;

	spin_lock_irqsave(&c->lock, flags);
	if (c->count < TBFRAME_MOCK_MAX_EVENTS) {
		c->events[c->count] = ev;
		c->reasons[c->count] = reason;
		c->count++;
	}
	spin_unlock_irqrestore(&c->lock, flags);
}

static void tbframe_mock_client_rx(void *ctx, struct tbframe_link *link,
				   struct tbframe_frame *frame)
{
	struct tbframe_mock_client *c = ctx;

	c->last_rx_len = frame->len;
	c->last_rx_pdf = frame->pdf;
	c->rx_count++;
	tbframe_mock_client_event(c, TBFRAME_EV_RX, 0);
}

static void tbframe_mock_client_tx_released(void *ctx,
					    struct tbframe_link *link)
{
	struct tbframe_mock_client *c = ctx;

	c->tx_released_count++;
	tbframe_mock_client_event(c, TBFRAME_EV_TX_RELEASED, 0);
}

static void tbframe_mock_client_link_up(void *ctx, struct tbframe_link *link,
					const struct tbframe_link_info *info)
{
	struct tbframe_mock_client *c = ctx;

	c->last_info = *info;
	c->up_count++;
	tbframe_mock_client_event(c, TBFRAME_EV_UP, 0);
}

static void tbframe_mock_client_link_down(void *ctx,
					  struct tbframe_link *link,
					  enum tbframe_down_reason reason)
{
	struct tbframe_mock_client *c = ctx;

	c->down_count++;
	tbframe_mock_client_event(c, TBFRAME_EV_DOWN, reason);
}

static const struct tbframe_client_ops tbframe_mock_client_ops = {
	.rx		= tbframe_mock_client_rx,
	.tx_released	= tbframe_mock_client_tx_released,
	.link_up	= tbframe_mock_client_link_up,
	.link_down	= tbframe_mock_client_link_down,
};

/* hw ops */

static int tbframe_mock_alloc_out_hopid(void *data)
{
	struct tbframe_mock *m = data;

	m->out_hopid_allocs++;
	return m->next_out_hopid++;
}

static void tbframe_mock_release_out_hopid(void *data, int hopid)
{
	struct tbframe_mock *m = data;

	m->out_hopid_releases++;
	m->last_release_out_hopid = hopid;
}

static int tbframe_mock_alloc_in_hopid(void *data, int hopid)
{
	struct tbframe_mock *m = data;

	m->in_hopid = hopid;
	m->last_alloc_in_hopid = hopid;
	m->in_hopid_allocs++;
	return hopid;
}

static void tbframe_mock_release_in_hopid(void *data, int hopid)
{
	struct tbframe_mock *m = data;

	tbframe_mock_hw_call(m, TBFRAME_HW_RELEASE_IN_HOPID);
	if (m->paths_on)
		m->released_hopid_while_paths_on = true;
	m->in_hopid = -1;
	m->last_release_in_hopid = hopid;
	m->in_hopid_releases++;
}

static int tbframe_mock_alloc_rings(void *data, u16 tx_entries,
				    u16 rx_entries, bool e2e)
{
	struct tbframe_mock *m = data;

	m->rings_alloced = true;
	m->rings_e2e = e2e;
	return 0;
}

static void tbframe_mock_start_rings(void *data)
{
	struct tbframe_mock *m = data;

	m->rings_started = true;
}

static void __tbframe_mock_deliver_peer_keepalive(struct tbframe_mock *m);

static void tbframe_mock_quiesce_tx(void *data)
{
	struct tbframe_mock *m = data;

	tbframe_mock_hw_call(m, TBFRAME_HW_QUIESCE_TX);
	if (m->deliver_on_quiesce) {
		m->deliver_on_quiesce = false;
		__tbframe_mock_deliver_peer_keepalive(m);
	}
}

static void tbframe_mock_stop_rings(void *data)
{
	struct tbframe_mock *m = data;

	tbframe_mock_hw_call(m, TBFRAME_HW_STOP_RINGS);
	m->rings_started = false;
	if (m->never_cancel)
		return;

	while (!list_empty(&m->rx_posted)) {
		struct ring_frame *rf = list_first_entry(&m->rx_posted,
							 struct ring_frame,
							 list);
		struct tbframe_frame_priv *f =
			container_of(rf, struct tbframe_frame_priv, rf);

		list_del_init(&rf->list);
		m->rx_posted_count--;
		tbframe_core_rx_complete(f, true, 0, 0, false);
	}
	while (!list_empty(&m->tx_queue)) {
		struct ring_frame *rf = list_first_entry(&m->tx_queue,
							 struct ring_frame,
							 list);
		struct tbframe_frame_priv *f =
			container_of(rf, struct tbframe_frame_priv, rf);

		list_del_init(&rf->list);
		tbframe_core_tx_complete(f, true);
	}
}

static void tbframe_mock_free_rings(void *data)
{
	struct tbframe_mock *m = data;

	tbframe_mock_hw_call(m, TBFRAME_HW_FREE_RINGS);
	if (m->paths_on)
		m->freed_rings_while_paths_on = true;
	m->rings_alloced = false;
}

static int tbframe_mock_post_rx(void *data, struct tbframe_frame_priv *f)
{
	struct tbframe_mock *m = data;

	if (!m->rings_alloced)
		return -ESHUTDOWN;
	if (!m->enable_seen)
		m->rx_posted_before_enable++;
	INIT_LIST_HEAD(&f->rf.list);
	list_add_tail(&f->rf.list, &m->rx_posted);
	m->rx_posted_count++;
	return 0;
}

/*
 * Model peer keepalive cadence: a live peer emits a keepalive frame on the
 * data ring once per verify interval, so our own keepalive going out is a
 * good proxy for "a peer frame is about to arrive". Deliver one into a posted
 * RX descriptor, carrying the PEER's session cookie (our own cookie would
 * read as a peer restart and supersede the session).
 *
 * @datapath_dead suppresses it, modelling exactly the field failure: the
 * control plane and the hop entries are healthy, the bulk path moves nothing.
 */
static void __tbframe_mock_deliver_peer_keepalive(struct tbframe_mock *m)
{
	struct tbframe_wire_keepalive keepalive = {
		.session_cookie = m->peer.session_cookie,
		.sequence = ++m->peer_keepalive_seq,
		.ack_cookie = m->peer_ack_cookie,
		.ack_sequence = m->peer_ack_sequence,
	};
	struct tbframe_frame_priv *f;
	struct ring_frame *rf;
	bool directional = m->peer.capabilities &
		TBFRAME_WIRE_CAP_KEEPALIVE_ACK;

	if (list_empty(&m->rx_posted))
		return;

	rf = list_first_entry(&m->rx_posted, struct ring_frame, list);
	list_del_init(&rf->list);
	m->rx_posted_count--;
	f = container_of(rf, struct tbframe_frame_priv, rf);
	if (m->stale_peer_keepalives) {
		m->stale_peer_keepalives--;
		keepalive.session_cookie = m->stale_peer_cookie;
		keepalive.ack_cookie = 0;
		keepalive.ack_sequence = 0;
	}
	if (directional)
		tbframe_wire_build_keepalive(f->frame.data, &keepalive);
	else
		tbframe_wire_put_le64(f->frame.data, keepalive.session_cookie);
	m->peer_keepalives++;
	tbframe_core_rx_complete(f, false,
				directional ? TBFRAME_KEEPALIVE_LEN :
				TBFRAME_WIRE_KEEPALIVE_LEGACY_SIZE,
				 TBFRAME_PDF_KEEPALIVE, false);
}

static void tbframe_mock_peer_keepalive(struct tbframe_mock *m)
{
	if (m->datapath_dead || m->peer_tx_dead)
		return;

	__tbframe_mock_deliver_peer_keepalive(m);
}

static int tbframe_mock_ring_tx(void *data, struct tbframe_frame_priv *f)
{
	struct tbframe_mock *m = data;
	bool keepalive = f->frame.pdf == TBFRAME_PDF_KEEPALIVE;

	if (!m->rings_started)
		return -ESHUTDOWN;

	if (keepalive) {
		struct tbframe_wire_keepalive sent;

		m->keepalives_sent++;
		memset(&sent, 0, sizeof(sent));
		if (f->frame.len == TBFRAME_KEEPALIVE_LEN)
			tbframe_wire_parse_keepalive(f->frame.data, &sent);
		if (f->frame.len == TBFRAME_KEEPALIVE_LEN &&
		    !m->datapath_dead && !m->local_tx_dead) {
			m->peer_ack_cookie = sent.session_cookie;
			m->peer_ack_sequence = sent.sequence;
		}
		tbframe_mock_peer_keepalive(m);
		/*
		 * Complete it the way the hardware does, so a keepalive never
		 * distorts the TX-ring arithmetic the window/budget tests
		 * measure. hold_keepalives opts out for tests that want to
		 * inspect the frame itself.
		 */
		if (!m->hold_keepalives && !m->tx_consumer_stalled) {
			tbframe_core_tx_complete(f, false);
			return 0;
		}
	}
	INIT_LIST_HEAD(&f->rf.list);
	list_add_tail(&f->rf.list, &m->tx_queue);
	m->tx_queued++;
	return 0;
}

static int tbframe_mock_enable_paths(void *data, int local_hopid,
				     int remote_hopid)
{
	struct tbframe_mock *m = data;

	m->enable_paths_calls++;
	m->enable_seen = true;
	m->paths_on = true;
	return m->enable_paths_err;
}

static int tbframe_mock_disable_paths(void *data, int local_hopid,
				      int remote_hopid)
{
	struct tbframe_mock *m = data;
	unsigned long flags;
	bool parked;

	if (m->must_be_quiet_before_disable) {
		struct tbframe_link *link = m->must_be_quiet_before_disable;

		spin_lock_irqsave(&link->lock, flags);
		parked = link->parked;
		spin_unlock_irqrestore(&link->lock, flags);
		if (!parked)
			m->disable_started_before_all_links_parked = true;
		if (work_busy(&link->session_work.work) ||
		    work_busy(&link->verify_work) ||
		    work_busy(&link->tx_released_work) ||
		    timer_pending(&link->verify_timer))
			m->disable_started_before_all_activity_stopped = true;
	}

	m->disable_paths_calls++;
	tbframe_mock_hw_call(m, TBFRAME_HW_DISABLE_PATHS);
	m->last_disable_remote_hopid = remote_hopid;
	if (m->disable_paths_err)
		return m->disable_paths_err;
	m->paths_on = false;
	return 0;
}

static int tbframe_mock_paths_active(void *data, int local_hopid,
				     int remote_hopid)
{
	struct tbframe_mock *m = data;

	m->paths_active_calls++;
	if (!m->paths_active_ret)
		m->paths_on = false;
	return m->paths_active_ret;
}

static int tbframe_mock_tx_snapshot(void *data,
				    struct tb_ring_snapshot *snapshot)
{
	struct tbframe_mock *m = data;

	memset(snapshot, 0, sizeof(*snapshot));
	snapshot->size = m->peer.rx_ring_entries;
	snapshot->hw_producer = m->keepalives_sent % snapshot->size;
	snapshot->hw_consumer = m->tx_consumer_stalled ? 0 :
		snapshot->hw_producer;
	snapshot->in_flight = m->tx_consumer_stalled ? m->tx_queued : 0;
	snapshot->options = BIT(31);
	snapshot->running = m->rings_started;
	snapshot->indices_valid = true;
	return 0;
}

static int
tbframe_mock_report_tx_stall(void *data,
			     const struct tb_ring_snapshot *first,
			     const struct tb_ring_snapshot *last,
			     bool control_healthy)
{
	struct tbframe_mock *m = data;

	if (!tb_nhi_runtime_recovery_evidence(first, last, control_healthy))
		return -EAGAIN;
	m->data_path_failure_reports++;
	return 0;
}

static int tbframe_mock_quarantine_paths(void *data, int local_hopid,
					 int remote_hopid)
{
	struct tbframe_mock *m = data;

	m->quarantine_requests++;
	m->rings_quarantined = true;
	m->quarantine_local_hopid = local_hopid;
	m->quarantine_remote_hopid = remote_hopid;
	return 0;
}

static void tbframe_mock_report_data_proven(void *data)
{
	struct tbframe_mock *m = data;

	m->data_proven_reports++;
}

static int tbframe_mock_control_request(void *data, const void *req,
					size_t req_len, void *resp,
					size_t resp_len,
					unsigned int timeout_ms)
{
	struct tbframe_mock *m = data;
	struct tbframe_wire_hello local;
	struct tbframe_wire_info info;
	u16 ack_op;
	int ret;

	ret = tbframe_wire_parse_hello(req, req_len, &local, &info);
	if (ret)
		return ret;
	if (m->req_count < TBFRAME_MOCK_MAX_REQS) {
		m->req_ops[m->req_count] = info.op;
		m->req_caps[m->req_count] = local.capabilities;
		m->req_count++;
	}
	m->last_request_tx_hopid = local.transmit_hopid;

	if (m->control_err)
		return m->control_err;
	if (m->fail_ready && info.op == TBFRAME_WIRE_OP_READY)
		return -ETIMEDOUT;
	if (info.op == TBFRAME_WIRE_OP_HELLO &&
	    m->delayed_hello_ack_pending) {
		if (resp_len < sizeof(m->delayed_hello_ack))
			return -ENOSPC;
		memcpy(resp, m->delayed_hello_ack,
		       sizeof(m->delayed_hello_ack));
		m->delayed_hello_ack_pending = false;
		return 0;
	}

	switch (info.op) {
	case TBFRAME_WIRE_OP_HELLO:
		ack_op = TBFRAME_WIRE_OP_HELLO_ACK;
		break;
	case TBFRAME_WIRE_OP_READY:
		ack_op = TBFRAME_WIRE_OP_READY_ACK;
		break;
	case TBFRAME_WIRE_OP_BYE:
		if (m->must_be_parked_before_bye) {
			struct tbframe_link *link = m->must_be_parked_before_bye;
			unsigned long flags;
			bool parked;

			spin_lock_irqsave(&link->lock, flags);
			parked = link->parked;
			spin_unlock_irqrestore(&link->lock, flags);
			if (!parked)
				m->bye_started_before_all_links_parked = true;
			if (work_busy(&link->session_work.work) ||
			    work_busy(&link->verify_work) ||
			    work_busy(&link->tx_released_work) ||
			    timer_pending(&link->verify_timer))
				m->bye_started_before_all_session_work_stopped = true;
		}
		/* Quiesce contract: BYE leaves after our TX drained but
		 * before any ring stop or path teardown, so the receiving
		 * side's rings still absorb our residue and ours theirs. */
		m->bye_rings_started = m->rings_started;
		m->bye_hw_calls_at = m->hw_call_count;
		m->bye_count++;
		ack_op = TBFRAME_WIRE_OP_BYE_ACK;
		break;
	default:
		return -EPROTO;
	}
	if (m->response_op)
		ack_op = m->response_op;
	ret = tbframe_wire_build_hello(resp, resp_len, &m->peer, ack_op,
				       info.op == TBFRAME_WIRE_OP_READY &&
				       m->ready_ack_u8_seq ? (u8)info.seq : info.seq,
				       m->route,
				       info.xdomain_sequence);
	if (ret >= 0 && info.op == TBFRAME_WIRE_OP_HELLO &&
	    m->delay_next_hello_ack) {
		memcpy(m->delayed_hello_ack, resp,
		       sizeof(m->delayed_hello_ack));
		m->delay_next_hello_ack = false;
		m->delayed_hello_ack_pending = true;
		return -ETIMEDOUT;
	}
	return ret < 0 ? ret : 0;
}

static int tbframe_mock_control_response(void *data, const void *resp,
					 size_t len)
{
	struct tbframe_mock *m = data;

	if (len > sizeof(m->last_response))
		return -EINVAL;
	memcpy(m->last_response, resp, len);
	m->have_response = true;
	return 0;
}

static bool tbframe_mock_reannounce(void *data)
{
	struct tbframe_mock *m = data;

	m->reannounce_calls++;
	return true;
}

static void tbframe_mock_link_attrs(void *data, u8 *width, u8 *speed)
{
	*width = 2;
	*speed = 20;
}

static bool tbframe_mock_match(void *data, const void *token)
{
	return true;
}

static const struct tbframe_hw_ops tbframe_mock_ops = {
	.alloc_out_hopid	= tbframe_mock_alloc_out_hopid,
	.release_out_hopid	= tbframe_mock_release_out_hopid,
	.alloc_in_hopid		= tbframe_mock_alloc_in_hopid,
	.release_in_hopid	= tbframe_mock_release_in_hopid,
	.alloc_rings		= tbframe_mock_alloc_rings,
	.start_rings		= tbframe_mock_start_rings,
	.quiesce_tx		= tbframe_mock_quiesce_tx,
	.stop_rings		= tbframe_mock_stop_rings,
	.free_rings		= tbframe_mock_free_rings,
	.post_rx		= tbframe_mock_post_rx,
	.ring_tx		= tbframe_mock_ring_tx,
	.enable_paths		= tbframe_mock_enable_paths,
	.disable_paths		= tbframe_mock_disable_paths,
	.paths_active		= tbframe_mock_paths_active,
	.tx_snapshot		= tbframe_mock_tx_snapshot,
	.report_tx_stall	= tbframe_mock_report_tx_stall,
	.quarantine_paths	= tbframe_mock_quarantine_paths,
	.report_data_proven	= tbframe_mock_report_data_proven,
	.control_request	= tbframe_mock_control_request,
	.control_response	= tbframe_mock_control_response,
	.reannounce		= tbframe_mock_reannounce,
	.link_attrs		= tbframe_mock_link_attrs,
	.match			= tbframe_mock_match,
};

/* fixture */

struct tbframe_mock_fixture {
	struct tbframe		tf;
	struct tbframe_mock	mock;
	struct tbframe_mock_client client;
	struct tbframe_link	*link;
	bool			link_destroyed;
};

#define TBFRAME_MOCK_ROUTE	0x301ull
#define TBFRAME_MOCK_RING	256
#define TBFRAME_MOCK_LOCAL_EUI64 0xfedcba0908070605ull

static int tbframe_mock_fixture_init(struct kunit *test,
				     struct tbframe_mock_fixture *fx)
{
	memset(fx, 0, sizeof(*fx));
	tbframe_state_init(&fx->tf);
	fx->tf.ring_entries = TBFRAME_MOCK_RING;
	fx->tf.e2e = false;
	fx->tf.keepalive = true;
	fx->tf.verify_ms = 5000;
	fx->tf.xmit_drain_ms = 200;
	fx->tf.teardown_warn_ms = 50;
	fx->tf.teardown_force_ms = 100;
	fx->tf.data_proof = TBFRAME_DATA_PROOF;
	fx->tf.wq = alloc_workqueue("tbframe-kunit",
				    WQ_MEM_RECLAIM | WQ_UNBOUND, 0);
	if (!fx->tf.wq)
		return -ENOMEM;

	INIT_LIST_HEAD(&fx->mock.tx_queue);
	INIT_LIST_HEAD(&fx->mock.rx_posted);
	fx->mock.in_hopid = -1;
	fx->mock.last_alloc_in_hopid = -1;
	fx->mock.last_release_in_hopid = -1;
	fx->mock.last_disable_remote_hopid = -1;
	fx->mock.route = TBFRAME_MOCK_ROUTE;
	fx->mock.paths_active_ret = 1;
	fx->mock.peer.proto_version = TBFRAME_WIRE_VERSION;
	fx->mock.peer.transmit_hopid = 9;
	fx->mock.peer.rx_ring_entries = TBFRAME_MOCK_RING;
	fx->mock.peer.capabilities = TBFRAME_WIRE_CAP_KEEPALIVE |
		TBFRAME_WIRE_CAP_KEEPALIVE_ACK;
	fx->mock.peer.gid_eui64 = 0xabcdef0102030405ull;
	fx->mock.peer.session_cookie = 0x1111222233334444ull;

	spin_lock_init(&fx->client.lock);
	fx->tf.client_ops = &tbframe_mock_client_ops;
	fx->tf.client_ctx = &fx->client;

	fx->link = tbframe_link_create(&fx->tf, &tbframe_mock_ops, &fx->mock,
				       TBFRAME_MOCK_ROUTE,
				       TBFRAME_MOCK_LOCAL_EUI64, false);
	if (IS_ERR(fx->link)) {
		destroy_workqueue(fx->tf.wq);
		return PTR_ERR(fx->link);
	}
	return 0;
}

static void tbframe_mock_fixture_exit(struct kunit *test,
				      struct tbframe_mock_fixture *fx)
{
	if (!fx->link_destroyed)
		tbframe_link_destroy(fx->link, TBFRAME_DOWN_CLOSED);
	destroy_workqueue(fx->tf.wq);
}

/* Drive one full session pass; with a healthy mock this reaches UP. */
static void tbframe_mock_link_up(struct kunit *test,
				 struct tbframe_mock_fixture *fx)
{
	tbframe_link_session_step(fx->link);
	KUNIT_ASSERT_EQ(test, 1u, fx->client.up_count);
}

/* Pop one posted RX descriptor for direct injection into the core. */
static __maybe_unused struct tbframe_frame_priv *
tbframe_mock_pop_rx(struct tbframe_mock_fixture *fx)
{
	struct ring_frame *rf;

	if (list_empty(&fx->mock.rx_posted))
		return NULL;
	rf = list_first_entry(&fx->mock.rx_posted, struct ring_frame, list);
	list_del_init(&rf->list);
	fx->mock.rx_posted_count--;
	return container_of(rf, struct tbframe_frame_priv, rf);
}

static __maybe_unused void
tbframe_mock_fill_keepalive(struct tbframe_frame_priv *f, u64 cookie, u64 seq,
			    u64 ack_cookie, u64 ack_seq)
{
	const struct tbframe_wire_keepalive keepalive = {
		.session_cookie = cookie,
		.sequence = seq,
		.ack_cookie = ack_cookie,
		.ack_sequence = ack_seq,
	};

	tbframe_wire_build_keepalive(f->frame.data, &keepalive);
}

/* Complete the oldest queued TX frame as the hardware would. */
static __maybe_unused bool
tbframe_mock_complete_tx(struct tbframe_mock_fixture *fx)
{
	struct ring_frame *rf;

	if (list_empty(&fx->mock.tx_queue))
		return false;
	rf = list_first_entry(&fx->mock.tx_queue, struct ring_frame, list);
	list_del_init(&rf->list);
	tbframe_core_tx_complete(container_of(rf, struct tbframe_frame_priv,
					      rf), false);
	return true;
}

/* How many control requests of @op the link has issued so far. */
static __maybe_unused unsigned int
tbframe_mock_count_req_op(const struct tbframe_mock *m, u16 op)
{
	unsigned int i, n = 0;

	for (i = 0; i < m->req_count; i++)
		if (m->req_ops[i] == op)
			n++;
	return n;
}

static __maybe_unused int
tbframe_mock_build_peer_msg(struct tbframe_mock_fixture *fx,
			    u16 op, u8 *buf, size_t size)
{
	return tbframe_wire_build_hello(buf, size, &fx->mock.peer, op, 0,
					TBFRAME_MOCK_ROUTE, 0);
}

#endif /* TBFRAME_TESTS_MOCK_H */
