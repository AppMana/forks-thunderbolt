// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit: tbframe intra-domain self-loop contract (docs/tb_same_host.md).
 *
 * Models one host whose two ports are cabled to each other: TWO links on
 * ONE tbframe instance, every UUID equal on both ends, only the routes
 * distinct. Unlike tbframe_mock.h's canned peer, the wire here is the
 * other link's real state machine: a control request sent by link A is
 * delivered to tbframe_link_handle_packet(B) and A receives whatever B's
 * handler actually produced (and vice versa). The XDomain header route is
 * rewritten to the receiver's route on both directions, exactly what the
 * hardware does when a packet crosses the cable (as-received route
 * identifies the ingress XDomain).
 *
 * Proves: distinct EUI-64 identity per loop end (shared derivation,
 * tbframe_identity.h), symmetric bring-up with no role election, no
 * session cross-talk, independent per-link data windows, and orderly
 * BYE teardown of one end observed by the other.
 * Run via tools/run-kunit.sh.
 */
#include <kunit/test.h>
#include <linux/slab.h>

#include "../tbframe_identity.h"
#include "../tbframe_priv.h"

#define SELFLOOP_RING		256
#define SELFLOOP_ROUTE_A	0x301ull
#define SELFLOOP_ROUTE_B	0x501ull
#define SELFLOOP_MAX_EV		32

/* One host UUID: both the local and the remote identity of BOTH links. */
static const u8 selfloop_uuid[16] = {
	0xde, 0xad, 0xbe, 0xef, 0x00, 0x01, 0x02, 0x03,
	0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b,
};

struct selfloop_fixture;

struct selfloop_end {
	struct selfloop_fixture *fx;
	int		idx;		/* 0 = A, 1 = B */
	u64		route;

	bool		rings_alloced;
	bool		rings_started;
	bool		paths_on;
	int		next_out_hopid;
	int		in_hopid;
	int		paths_active_ret;

	struct list_head tx_queue;
	struct list_head rx_posted;
	unsigned int	tx_queued;
	unsigned int	rx_posted_count;

	u8		last_response[TBFRAME_WIRE_HELLO_MSG_SIZE];
	bool		have_response;
	bool		inject_restart_before_hello_ack;
	bool		restart_injected;

	/* client events for THIS link */
	unsigned int	up_count;
	unsigned int	down_count;
	unsigned int	rx_count;
	unsigned int	tx_released_count;
	int		last_down_reason;
	u16		last_rx_len;
	u8		last_rx_pdf;
	struct tbframe_link_info last_info;
};

struct selfloop_fixture {
	struct tbframe		tf;
	struct selfloop_end	end[2];
	struct tbframe_link	*link[2];
	bool			destroyed[2];
};

static struct selfloop_end *selfloop_peer(struct selfloop_end *e)
{
	return &e->fx->end[e->idx ^ 1];
}

static struct tbframe_link *selfloop_peer_link(struct selfloop_end *e)
{
	return e->fx->link[e->idx ^ 1];
}

/* The hardware's route rewrite: as-received route = receiver's route. */
static void selfloop_patch_route(u8 *msg, u64 route)
{
	tbframe_wire_put_le32(msg, route >> 32);
	tbframe_wire_put_le32(msg + 4, route & 0xffffffffull);
}

/* client ops: record per-end by matching the link pointer */

static struct selfloop_end *selfloop_end_of(struct selfloop_fixture *fx,
					    struct tbframe_link *link)
{
	return link == fx->link[0] ? &fx->end[0] : &fx->end[1];
}

static void selfloop_client_rx(void *ctx, struct tbframe_link *link,
			       struct tbframe_frame *frame)
{
	struct selfloop_end *e = selfloop_end_of(ctx, link);

	e->last_rx_len = frame->len;
	e->last_rx_pdf = frame->pdf;
	e->rx_count++;
}

static void selfloop_client_tx_released(void *ctx, struct tbframe_link *link)
{
	selfloop_end_of(ctx, link)->tx_released_count++;
}

static void selfloop_client_link_up(void *ctx, struct tbframe_link *link,
				    const struct tbframe_link_info *info)
{
	struct selfloop_end *e = selfloop_end_of(ctx, link);

	e->last_info = *info;
	e->up_count++;
}

static void selfloop_client_link_down(void *ctx, struct tbframe_link *link,
				      enum tbframe_down_reason reason)
{
	struct selfloop_end *e = selfloop_end_of(ctx, link);

	e->last_down_reason = reason;
	e->down_count++;
}

static const struct tbframe_client_ops selfloop_client_ops = {
	.rx		= selfloop_client_rx,
	.tx_released	= selfloop_client_tx_released,
	.link_up	= selfloop_client_link_up,
	.link_down	= selfloop_client_link_down,
};

/* hw ops: per-end ring/path state, control plane looped to the peer */

static int selfloop_alloc_out_hopid(void *data)
{
	struct selfloop_end *e = data;

	return e->next_out_hopid++;
}

static void selfloop_release_out_hopid(void *data, int hopid) { }

static int selfloop_alloc_in_hopid(void *data, int hopid)
{
	struct selfloop_end *e = data;

	e->in_hopid = hopid;
	return hopid;
}

static void selfloop_release_in_hopid(void *data, int hopid)
{
	struct selfloop_end *e = data;

	e->in_hopid = -1;
}

static int selfloop_alloc_rings(void *data, u16 tx_entries, u16 rx_entries,
				bool e2e)
{
	struct selfloop_end *e = data;

	e->rings_alloced = true;
	return 0;
}

static void selfloop_start_rings(void *data)
{
	struct selfloop_end *e = data;

	e->rings_started = true;
}

static void selfloop_quiesce_tx(void *data) { }

static void selfloop_stop_rings(void *data)
{
	struct selfloop_end *e = data;

	e->rings_started = false;
	while (!list_empty(&e->rx_posted)) {
		struct ring_frame *rf = list_first_entry(&e->rx_posted,
							 struct ring_frame,
							 list);
		struct tbframe_frame_priv *f =
			container_of(rf, struct tbframe_frame_priv, rf);

		list_del_init(&rf->list);
		e->rx_posted_count--;
		tbframe_core_rx_complete(f, true, 0, 0, false);
	}
	while (!list_empty(&e->tx_queue)) {
		struct ring_frame *rf = list_first_entry(&e->tx_queue,
							 struct ring_frame,
							 list);
		struct tbframe_frame_priv *f =
			container_of(rf, struct tbframe_frame_priv, rf);

		list_del_init(&rf->list);
		tbframe_core_tx_complete(f, true);
	}
}

static void selfloop_free_rings(void *data)
{
	struct selfloop_end *e = data;

	e->rings_alloced = false;
}

static int selfloop_post_rx(void *data, struct tbframe_frame_priv *f)
{
	struct selfloop_end *e = data;

	if (!e->rings_alloced)
		return -ESHUTDOWN;
	INIT_LIST_HEAD(&f->rf.list);
	list_add_tail(&f->rf.list, &e->rx_posted);
	e->rx_posted_count++;
	return 0;
}

static int selfloop_ring_tx(void *data, struct tbframe_frame_priv *f)
{
	struct selfloop_end *e = data;

	if (!e->rings_started)
		return -ESHUTDOWN;
	INIT_LIST_HEAD(&f->rf.list);
	list_add_tail(&f->rf.list, &e->tx_queue);
	e->tx_queued++;
	return 0;
}

static int selfloop_enable_paths(void *data, int local_hopid, int remote_hopid)
{
	struct selfloop_end *e = data;

	e->paths_on = true;
	return 0;
}

static int selfloop_disable_paths(void *data, int local_hopid,
				  int remote_hopid)
{
	struct selfloop_end *e = data;

	e->paths_on = false;
	return 0;
}

static int selfloop_paths_active(void *data, int local_hopid, int remote_hopid)
{
	struct selfloop_end *e = data;

	return e->paths_active_ret;
}

/*
 * The looped control plane. A request leaving this end is handled by the
 * peer link's real inbound path; a reply the peer chose to produce (its
 * control_response) comes back as this request's response, with the ACK
 * observation pass through our own handler first, exactly like the real
 * requester's protocol-handler-before-response-matching ordering. A
 * withheld ack (peer returned no response) is a request timeout.
 */
static int selfloop_control_request(void *data, const void *req,
				    size_t req_len, void *resp,
				    size_t resp_len, unsigned int timeout_ms)
{
	struct selfloop_end *e = data;
	struct selfloop_end *peer = selfloop_peer(e);
	struct tbframe_wire_hello request_hello;
	struct tbframe_wire_info request_info;
	u8 buf[TBFRAME_WIRE_HELLO_MSG_SIZE];
	u8 stale_response[TBFRAME_WIRE_HELLO_MSG_SIZE];
	int ret;

	if (req_len > sizeof(buf) || resp_len > sizeof(buf))
		return -EINVAL;

	memcpy(buf, req, req_len);
	selfloop_patch_route(buf, peer->route);
	peer->have_response = false;
	tbframe_link_handle_packet(selfloop_peer_link(e), buf, req_len);
	if (!peer->have_response)
		return -ETIMEDOUT;

	/*
	 * Model the real control-ring ordering where an unsolicited HELLO can be
	 * dispatched while a synchronous HELLO request is still waiting for its
	 * correlated response.  Preserve the already-built (now stale) ACK,
	 * advance the peer's local lifetime, and dispatch its new HELLO before
	 * allowing the old ACK to return to the requester.
	 */
	ret = tbframe_wire_parse_hello(req, req_len, &request_hello,
				       &request_info);
	if (!ret && request_info.op == TBFRAME_WIRE_OP_HELLO &&
	    e->inject_restart_before_hello_ack && !e->restart_injected) {
		struct tbframe_link *peer_link = selfloop_peer_link(e);
		struct tbframe_wire_hello restarted = { };

		memcpy(stale_response, peer->last_response,
		       sizeof(stale_response));
		e->restart_injected = true;
		peer_link->local_cookie ^= BIT_ULL(41);
		if (!peer_link->local_cookie)
			peer_link->local_cookie = 1;
		restarted.proto_version = TBFRAME_WIRE_VERSION;
		restarted.transmit_hopid = peer_link->local_hopid;
		restarted.rx_ring_entries = e->fx->tf.ring_entries;
		restarted.capabilities = TBFRAME_WIRE_CAP_KEEPALIVE;
		restarted.gid_eui64 = peer_link->local_gid_eui64;
		restarted.session_cookie = peer_link->local_cookie;
		ret = tbframe_wire_build_hello(buf, sizeof(buf), &restarted,
					       TBFRAME_WIRE_OP_HELLO, 0,
					       e->route, 0);
		if (ret < 0)
			return ret;
		tbframe_link_handle_packet(e->fx->link[e->idx], buf,
					   sizeof(buf));
		memcpy(peer->last_response, stale_response,
		       sizeof(stale_response));
	}

	memcpy(buf, peer->last_response, sizeof(buf));
	selfloop_patch_route(buf, e->route);
	tbframe_link_handle_packet(e->fx->link[e->idx], buf, sizeof(buf));
	memcpy(resp, buf, resp_len);
	return 0;
}

static int selfloop_control_response(void *data, const void *resp, size_t len)
{
	struct selfloop_end *e = data;

	if (len > sizeof(e->last_response))
		return -EINVAL;
	memcpy(e->last_response, resp, len);
	e->have_response = true;
	return 0;
}

static bool selfloop_reannounce(void *data)
{
	return true;
}

static void selfloop_link_attrs(void *data, u8 *width, u8 *speed)
{
	/* Intra-domain loops never bond (xdomain.c): single lane. */
	*width = 1;
	*speed = 20;
}

static bool selfloop_match(void *data, const void *token)
{
	return true;
}

static const struct tbframe_hw_ops selfloop_ops = {
	.alloc_out_hopid	= selfloop_alloc_out_hopid,
	.release_out_hopid	= selfloop_release_out_hopid,
	.alloc_in_hopid		= selfloop_alloc_in_hopid,
	.release_in_hopid	= selfloop_release_in_hopid,
	.alloc_rings		= selfloop_alloc_rings,
	.start_rings		= selfloop_start_rings,
	.quiesce_tx		= selfloop_quiesce_tx,
	.stop_rings		= selfloop_stop_rings,
	.free_rings		= selfloop_free_rings,
	.post_rx		= selfloop_post_rx,
	.ring_tx		= selfloop_ring_tx,
	.enable_paths		= selfloop_enable_paths,
	.disable_paths		= selfloop_disable_paths,
	.paths_active		= selfloop_paths_active,
	.control_request	= selfloop_control_request,
	.control_response	= selfloop_control_response,
	.reannounce		= selfloop_reannounce,
	.link_attrs		= selfloop_link_attrs,
	.match			= selfloop_match,
};

/* wire: move one queued TX frame across the loop into the peer's RX */

static bool selfloop_deliver_one(struct selfloop_fixture *fx, int from)
{
	struct selfloop_end *src = &fx->end[from];
	struct selfloop_end *dst = &fx->end[from ^ 1];
	struct tbframe_frame_priv *ftx, *frx;
	struct ring_frame *rf;
	u16 len;
	u8 pdf;

	if (list_empty(&src->tx_queue) || list_empty(&dst->rx_posted))
		return false;

	rf = list_first_entry(&src->tx_queue, struct ring_frame, list);
	list_del_init(&rf->list);
	ftx = container_of(rf, struct tbframe_frame_priv, rf);
	len = ftx->frame.len;
	pdf = ftx->frame.pdf;

	rf = list_first_entry(&dst->rx_posted, struct ring_frame, list);
	list_del_init(&rf->list);
	dst->rx_posted_count--;
	frx = container_of(rf, struct tbframe_frame_priv, rf);
	if (len && ftx->frame.data && frx->frame.data)
		memcpy(frx->frame.data, ftx->frame.data, len);

	tbframe_core_tx_complete(ftx, false);
	tbframe_core_rx_complete(frx, false, len, pdf, false);
	return true;
}

/* fixture */

static int selfloop_fixture_init(struct kunit *test,
				 struct selfloop_fixture *fx)
{
	int i;

	memset(fx, 0, sizeof(*fx));
	tbframe_state_init(&fx->tf);
	fx->tf.ring_entries = SELFLOOP_RING;
	fx->tf.e2e = false;
	fx->tf.keepalive = true;
	fx->tf.verify_ms = 5000;
	fx->tf.xmit_drain_ms = 200;
	fx->tf.teardown_warn_ms = 50;
	fx->tf.teardown_force_ms = 100;
	fx->tf.wq = alloc_workqueue("tbframe-selfloop",
				    WQ_MEM_RECLAIM | WQ_UNBOUND, 0);
	if (!fx->tf.wq)
		return -ENOMEM;

	fx->tf.client_ops = &selfloop_client_ops;
	fx->tf.client_ctx = fx;

	for (i = 0; i < 2; i++) {
		struct selfloop_end *e = &fx->end[i];

		e->fx = fx;
		e->idx = i;
		e->route = i ? SELFLOOP_ROUTE_B : SELFLOOP_ROUTE_A;
		e->in_hopid = -1;
		e->paths_active_ret = 1;
		INIT_LIST_HEAD(&e->tx_queue);
		INIT_LIST_HEAD(&e->rx_posted);
	}

	for (i = 0; i < 2; i++) {
		/* The shipped derivation with every UUID identical. */
		u64 eui = tbframe_identity_eui64(selfloop_uuid, selfloop_uuid,
						 fx->end[i].route);

		fx->link[i] = tbframe_link_create(&fx->tf, &selfloop_ops,
						  &fx->end[i],
						  fx->end[i].route, eui,
						  false);
		if (IS_ERR(fx->link[i])) {
			int ret = PTR_ERR(fx->link[i]);

			if (i)
				tbframe_link_destroy(fx->link[0],
						     TBFRAME_DOWN_CLOSED);
			destroy_workqueue(fx->tf.wq);
			return ret;
		}
	}
	return 0;
}

static void selfloop_fixture_exit(struct kunit *test,
				  struct selfloop_fixture *fx)
{
	int i;

	for (i = 0; i < 2; i++)
		if (!fx->destroyed[i])
			tbframe_link_destroy(fx->link[i], TBFRAME_DOWN_CLOSED);
	destroy_workqueue(fx->tf.wq);
}

/*
 * Drive both symmetric sessions to UP. The protocol is retry-based (READY
 * acks are withheld until the responder's paths are enabled), so the
 * canonical dance is step(A) -> step(B) -> step(A); bound it and let the
 * queued session work fill any gaps.
 */
static void selfloop_bring_up(struct kunit *test, struct selfloop_fixture *fx)
{
	int i;

	for (i = 0; i < 8; i++) {
		if (fx->end[0].up_count && fx->end[1].up_count)
			break;
		tbframe_link_session_step(fx->link[0]);
		tbframe_link_session_step(fx->link[1]);
		flush_workqueue(fx->tf.wq);
	}
	KUNIT_ASSERT_EQ(test, 1u, fx->end[0].up_count);
	KUNIT_ASSERT_EQ(test, 1u, fx->end[1].up_count);
}

static void selfloop_drive_sessions(struct selfloop_fixture *fx)
{
	int i;

	for (i = 0; i < 16; i++) {
		tbframe_link_session_step(fx->link[0]);
		tbframe_link_session_step(fx->link[1]);
		flush_workqueue(fx->tf.wq);
		if (fx->link[0]->state == TBFRAME_STATE_UP &&
		    fx->link[1]->state == TBFRAME_STATE_UP &&
		    !fx->link[0]->needs_down && !fx->link[1]->needs_down)
			break;
	}
}

static void selfloop_send_keepalive(struct kunit *test,
				    struct selfloop_fixture *fx, int from)
{
	struct tbframe_link *link = fx->link[from];
	struct tbframe_frame *f;

	KUNIT_ASSERT_EQ(test, 0,
			tbframe_alloc_frame(link, TBFRAME_KEEPALIVE_LEN, true,
					    &f));
	tbframe_wire_put_le64(f->data, link->local_cookie);
	f->len = TBFRAME_KEEPALIVE_LEN;
	f->pdf = TBFRAME_PDF_KEEPALIVE;
	KUNIT_ASSERT_EQ(test, 0, tbframe_xmit(link, f));
	KUNIT_ASSERT_TRUE(test, selfloop_deliver_one(fx, from));
}

/* tests */

/* Same host UUID on every side: identity must still be per-link unique. */
static void selfloop_identity_distinct(struct kunit *test)
{
	u64 a = tbframe_identity_eui64(selfloop_uuid, selfloop_uuid,
				       SELFLOOP_ROUTE_A);
	u64 b = tbframe_identity_eui64(selfloop_uuid, selfloop_uuid,
				       SELFLOOP_ROUTE_B);

	KUNIT_EXPECT_NE(test, a, b);
	/* Stable (boot-to-boot determinism is what the GID plan rests on). */
	KUNIT_EXPECT_EQ(test, a,
			tbframe_identity_eui64(selfloop_uuid, selfloop_uuid,
					       SELFLOOP_ROUTE_A));
	/* Same host part (bits 63..40 mirror the 24-bit host hash). */
	KUNIT_EXPECT_EQ(test, a >> 40, b >> 40);
	/* Modified EUI-64 shape: ff:fe in the middle. */
	KUNIT_EXPECT_EQ(test, 0xfffeull, (a >> 24) & 0xffffull);
}

static void selfloop_dual_bringup(struct kunit *test)
{
	struct selfloop_fixture *fx = test->priv;

	selfloop_bring_up(test, fx);

	/* Each end negotiated against the OTHER end's real identity. */
	KUNIT_EXPECT_EQ(test, fx->end[0].last_info.gid_eui64,
			fx->end[1].last_info.local_gid_eui64);
	KUNIT_EXPECT_EQ(test, fx->end[1].last_info.gid_eui64,
			fx->end[0].last_info.local_gid_eui64);
	KUNIT_EXPECT_NE(test, fx->end[0].last_info.gid_eui64,
			fx->end[0].last_info.local_gid_eui64);

	/* Each end's info carries ITS OWN route (the udev tie-break input). */
	KUNIT_EXPECT_EQ(test, (u64)SELFLOOP_ROUTE_A,
			fx->end[0].last_info.route);
	KUNIT_EXPECT_EQ(test, (u64)SELFLOOP_ROUTE_B,
			fx->end[1].last_info.route);

	/* Distinct link names keyed by route. */
	KUNIT_EXPECT_STREQ(test, "tbframe0x301",
			   tbframe_link_name(fx->link[0]));
	KUNIT_EXPECT_STREQ(test, "tbframe0x501",
			   tbframe_link_name(fx->link[1]));

	/* Both ends fully primed and pathed, windows independent. */
	KUNIT_EXPECT_TRUE(test, fx->end[0].paths_on);
	KUNIT_EXPECT_TRUE(test, fx->end[1].paths_on);
	KUNIT_EXPECT_EQ(test, (u16)(SELFLOOP_RING - TBFRAME_CTRL_RESERVE),
			fx->end[0].last_info.data_window);
	KUNIT_EXPECT_EQ(test, (u16)(SELFLOOP_RING - TBFRAME_CTRL_RESERVE),
			fx->end[1].last_info.data_window);
	/* Loop links are single-lane by contract (no bonding on loops). */
	KUNIT_EXPECT_EQ(test, 1, fx->end[0].last_info.width);
}

/*
 * Requester and responder HELLO observations are independent.  If a peer's
 * new unsolicited HELLO is dispatched while an older correlated HELLO_ACK is
 * still returning, the old requester reply must not overwrite the newer
 * responder observation and program paths for the retired peer lifetime.
 */
static void selfloop_stale_hello_ack_cannot_overwrite_new_hello(struct kunit *test)
{
	struct selfloop_fixture *fx = test->priv;
	u64 initial_peer_cookie = fx->link[1]->local_cookie;

	fx->end[0].inject_restart_before_hello_ack = true;
	tbframe_link_session_step(fx->link[0]);

	KUNIT_ASSERT_TRUE(test, fx->end[0].restart_injected);
	KUNIT_ASSERT_NE(test, initial_peer_cookie,
			fx->link[1]->local_cookie);
	KUNIT_EXPECT_FALSE(test, fx->link[0]->hello_done);
	KUNIT_EXPECT_FALSE(test, fx->link[0]->rings_up);
	KUNIT_EXPECT_NE(test, initial_peer_cookie,
			fx->link[0]->remote_cookie);
}

static void selfloop_bidirectional_data(struct kunit *test)
{
	struct selfloop_fixture *fx = test->priv;
	struct tbframe_frame *f;
	int dir;

	selfloop_bring_up(test, fx);

	/* One data frame each way, delivered independently. */
	for (dir = 0; dir < 2; dir++) {
		KUNIT_ASSERT_EQ(test, 0,
				tbframe_alloc_frame(fx->link[dir], 64 + dir,
						    false, &f));
		memset(f->data, 0xa0 + dir, 64 + dir);
		f->len = 64 + dir;
		f->pdf = TBFRAME_PDF_DATA;
		KUNIT_ASSERT_EQ(test, 0, tbframe_xmit(fx->link[dir], f));
	}
	KUNIT_EXPECT_TRUE(test, selfloop_deliver_one(fx, 0));
	KUNIT_EXPECT_TRUE(test, selfloop_deliver_one(fx, 1));
	flush_workqueue(fx->tf.wq);

	KUNIT_EXPECT_EQ(test, 1u, fx->end[1].rx_count);
	KUNIT_EXPECT_EQ(test, 64, fx->end[1].last_rx_len);
	KUNIT_EXPECT_EQ(test, 1u, fx->end[0].rx_count);
	KUNIT_EXPECT_EQ(test, 65, fx->end[0].last_rx_len);
	KUNIT_EXPECT_EQ(test, (u8)TBFRAME_PDF_DATA, fx->end[0].last_rx_pdf);

	/*
	 * tx_released is the window-reopened wake, not a per-completion
	 * event: one in-flight frame never closes the window, so neither
	 * end may see a spurious wake from the other's completions.
	 */
	KUNIT_EXPECT_EQ(test, 0u, fx->end[0].tx_released_count);
	KUNIT_EXPECT_EQ(test, 0u, fx->end[1].tx_released_count);
}

/*
 * A local verify restart advances only the initiator's identity.  The peer
 * observes BYE/HELLO through its independent responder state machine and
 * must converge on that identity without advancing its own in response.
 * Finally move real keepalives both ways: matching control-plane HELLOs are
 * insufficient if either data-plane cookie is still one generation behind.
 */
static void selfloop_asymmetric_verify_reauthenticates(struct kunit *test)
{
	struct selfloop_fixture *fx = test->priv;
	u64 local_cookie[2];
	u64 bad_cookie[2];
	int i;

	selfloop_bring_up(test, fx);
	for (i = 0; i < 2; i++) {
		local_cookie[i] = fx->link[i]->local_cookie;
		bad_cookie[i] = fx->link[i]->data_rx_bad_cookie;
	}

	/* End A alone detects a locally observed dead path. */
	fx->end[0].paths_active_ret = 0;
	tbframe_link_verify_step(fx->link[0]);
	fx->end[0].paths_active_ret = 1;
	KUNIT_ASSERT_TRUE(test, fx->link[0]->needs_down);
	KUNIT_ASSERT_EQ(test, (int)TBFRAME_DOWN_VERIFY,
			fx->link[0]->down_reason);

	selfloop_drive_sessions(fx);

	KUNIT_ASSERT_EQ(test, (int)TBFRAME_STATE_UP, fx->link[0]->state);
	KUNIT_ASSERT_EQ(test, (int)TBFRAME_STATE_UP, fx->link[1]->state);
	KUNIT_EXPECT_NE(test, local_cookie[0], fx->link[0]->local_cookie);
	KUNIT_EXPECT_EQ(test, local_cookie[1], fx->link[1]->local_cookie);
	KUNIT_EXPECT_EQ(test, fx->link[1]->local_cookie,
			fx->link[0]->remote_cookie);
	KUNIT_EXPECT_EQ(test, fx->link[0]->local_cookie,
			fx->link[1]->remote_cookie);

	selfloop_send_keepalive(test, fx, 0);
	selfloop_send_keepalive(test, fx, 1);
	flush_workqueue(fx->tf.wq);
	for (i = 0; i < 2; i++) {
		KUNIT_EXPECT_EQ(test, bad_cookie[i],
				fx->link[i]->data_rx_bad_cookie);
		KUNIT_EXPECT_TRUE(test, fx->link[i]->data_proven);
	}
}

static void selfloop_bye_teardown(struct kunit *test)
{
	struct selfloop_fixture *fx = test->priv;

	selfloop_bring_up(test, fx);

	/* Destroy end A; end B must observe an orderly LOGOUT, not a hang. */
	tbframe_link_destroy(fx->link[0], TBFRAME_DOWN_CLOSED);
	fx->destroyed[0] = true;
	flush_workqueue(fx->tf.wq);

	KUNIT_EXPECT_EQ(test, 1u, fx->end[1].down_count);
	KUNIT_EXPECT_EQ(test, (int)TBFRAME_DOWN_LOGOUT,
			fx->end[1].last_down_reason);

	/* The survivor refuses traffic until a peer returns. */
	{
		struct tbframe_frame *f;

		KUNIT_EXPECT_EQ(test, -ENETDOWN,
				tbframe_alloc_frame(fx->link[1], 64, false,
						    &f));
	}
}

static int selfloop_test_init(struct kunit *test)
{
	struct selfloop_fixture *fx;
	int ret;

	fx = kunit_kzalloc(test, sizeof(*fx), GFP_KERNEL);
	if (!fx)
		return -ENOMEM;
	ret = selfloop_fixture_init(test, fx);
	if (ret)
		return ret;
	test->priv = fx;
	return 0;
}

static void selfloop_test_exit(struct kunit *test)
{
	selfloop_fixture_exit(test, test->priv);
}

static struct kunit_case selfloop_cases[] = {
	KUNIT_CASE(selfloop_identity_distinct),
	KUNIT_CASE(selfloop_stale_hello_ack_cannot_overwrite_new_hello),
	KUNIT_CASE(selfloop_dual_bringup),
	KUNIT_CASE(selfloop_bidirectional_data),
	KUNIT_CASE(selfloop_asymmetric_verify_reauthenticates),
	KUNIT_CASE(selfloop_bye_teardown),
	{}
};

static struct kunit_suite selfloop_suite = {
	.name = "tbframe_selfloop",
	.init = selfloop_test_init,
	.exit = selfloop_test_exit,
	.test_cases = selfloop_cases,
};
kunit_test_suite(selfloop_suite);

MODULE_LICENSE("GPL");
