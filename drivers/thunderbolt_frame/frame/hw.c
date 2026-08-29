// SPDX-License-Identifier: GPL-2.0
/*
 * tbframe hardware backend: the only file that touches tb_ring/tb_xdomain.
 * Everything above it (core.c) sees struct tbframe_hw_ops.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/dma-mapping.h>
#include <linux/module.h>
#include <linux/slab.h>

#include "tbframe_priv.h"

struct tbframe_hw {
	struct tb_xdomain	*xd;
	struct tb_ring		*tx_ring;
	struct tb_ring		*rx_ring;
	/* intra-domain loop control plane (see tbframe_hw_loop_*) */
	struct tbframe		*tf;
	struct list_head	loop_node;
	bool			loop;
	u8			*loop_capture;
	size_t			loop_capture_len;
	bool			loop_captured;
};

/*
 * Intra-domain loop control plane. Both ends of the cable are this kernel
 * (xd->remote_uuid == xd->local_uuid, enumerated via thunderbolt.loop_routes),
 * and on the boards that need such a rig the resident ICM firmware answers
 * ring-0 XDomain protocol packets itself instead of forwarding them
 * (packet-trace proven), so tb_xdomain_request() can never work. It also
 * never needs to: a control request from one end is delivered synchronously
 * to the sibling end's real inbound handler with the XDomain header route
 * rewritten to the receiver's route -- exactly the rewrite the hardware
 * performs for a packet crossing the cable, and exactly the wire model the
 * tbframe_selfloop KUnit suite proves the session layer against. Only the
 * DMA-ring data path touches the physical cable.
 *
 * One global mutex serializes loop control exchanges; requests are rare
 * (session handshake, keepalive-independent) and both ends requesting
 * concurrently must not deadlock on per-end state.
 */
static DEFINE_MUTEX(tbframe_loop_lock);
static LIST_HEAD(tbframe_loop_hws);

static struct tbframe_hw *tbframe_hw_loop_sibling(struct tbframe_hw *hw)
{
	struct tbframe_hw *other;

	list_for_each_entry(other, &tbframe_loop_hws, loop_node)
		if (other != hw &&
		    uuid_equal(other->xd->local_uuid, hw->xd->local_uuid))
			return other;
	return NULL;
}

/* The hardware's route rewrite: as-received header route = receiver's. */
static void tbframe_hw_loop_patch_route(u8 *msg, size_t len, u64 route)
{
	if (len < 8)
		return;
	tbframe_wire_put_le32(msg, route >> 32);
	tbframe_wire_put_le32(msg + 4, route & 0xffffffffull);
}

/*
 * Per-ring MSI-X interrupt moderation for the data rings. The NHI driver
 * programs 128 us into every vector at probe (nhi_enable_int_throttling),
 * which gates TX-completion (window release) and RX delivery by up to that
 * interval; the legacy driver zeroed it for its data rings via the fork's
 * optional tb_ring_throttling() export (nhi_interrupt_throttle_ns=0 was its
 * default) and measured single-digit-us ping-pong latency where the tbframe
 * stack measures ~64 us. Sentinel -1 leaves the hardware/driver default
 * untouched; 0 disables moderation; otherwise the interval in ns
 * (256 ns granularity). Runtime-writable: applies at the next session's
 * ring allocation. Resolved via symbol_get so
 * tbframe still loads on a core without the export.
 */
static int data_ring_throttle_ns;
module_param(data_ring_throttle_ns, int, 0644);
MODULE_PARM_DESC(data_ring_throttle_ns,
		 "Data-ring MSI-X interrupt moderation in ns (0 = disable, default; -1 = leave NHI default)");

extern int tb_ring_throttling(struct tb_ring *ring, unsigned int interval_nsec);

static void tbframe_hw_apply_throttling(struct tbframe_hw *hw)
{
	typeof(&tb_ring_throttling) fn;
	int interval = READ_ONCE(data_ring_throttle_ns);
	int ret;

	if (interval < 0)
		return;

	fn = symbol_get(tb_ring_throttling);
	if (!fn) {
		pr_warn_once("data_ring_throttle_ns set but tb_ring_throttling() is not exported by this thunderbolt core; ignoring\n");
		return;
	}
	ret = fn(hw->tx_ring, interval);
	ret = ret ?: fn(hw->rx_ring, interval);
	if (ret)
		pr_warn("ring throttling %d ns failed: %d\n", interval, ret);
	symbol_put(tb_ring_throttling);
}

static struct device *tbframe_hw_dma_dev(struct tbframe_hw *hw)
{
	return &hw->xd->tb->nhi->pdev->dev;
}

static int tbframe_hw_alloc_out_hopid(void *data)
{
	struct tbframe_hw *hw = data;

	return tb_xdomain_alloc_out_hopid(hw->xd, -1);
}

static void tbframe_hw_release_out_hopid(void *data, int hopid)
{
	struct tbframe_hw *hw = data;

	if (hopid >= 0)
		tb_xdomain_release_out_hopid(hw->xd, hopid);
}

static int tbframe_hw_alloc_in_hopid(void *data, int hopid)
{
	struct tbframe_hw *hw = data;
	int ret;

	/* The RX HopID must equal what the peer announced it transmits on. */
	ret = tb_xdomain_alloc_in_hopid(hw->xd, hopid);
	if (ret != hopid) {
		if (ret >= 0) {
			tb_xdomain_release_in_hopid(hw->xd, ret);
			ret = -EBUSY;
		}
		return ret;
	}
	return ret;
}

static void tbframe_hw_release_in_hopid(void *data, int hopid)
{
	struct tbframe_hw *hw = data;

	if (hopid >= 0)
		tb_xdomain_release_in_hopid(hw->xd, hopid);
}

static int tbframe_hw_alloc_rings(void *data, u16 tx_entries, u16 rx_entries,
				  bool e2e)
{
	struct tbframe_hw *hw = data;
	unsigned int flags = RING_FLAG_FRAME | (e2e ? RING_FLAG_E2E : 0);
	int e2e_tx_hop = 0;

	/*
	 * The protocol session may restart while the device-facing DMA storage
	 * remains the same. Keeping the allocation makes every address cached by
	 * the ring engine valid until the service itself is removed.
	 */
	if (hw->tx_ring || hw->rx_ring) {
		if (!hw->tx_ring || !hw->rx_ring)
			return -EUCLEAN;
		if (hw->tx_ring->size != tx_entries ||
		    hw->rx_ring->size != rx_entries ||
		    hw->tx_ring->flags != flags || hw->rx_ring->flags != flags)
			return -EINVAL;
		return 0;
	}

	hw->tx_ring = tb_ring_alloc_tx(hw->xd->tb->nhi, -1, tx_entries, flags);
	if (!hw->tx_ring)
		return -ENOMEM;

	/* E2E pairing convention: the RX ring is coupled to the local TX
	 * ring's HopID of the same duplex link. */
	if (e2e)
		e2e_tx_hop = hw->tx_ring->hop;

	hw->rx_ring = tb_ring_alloc_rx(hw->xd->tb->nhi, -1, rx_entries, flags,
				       e2e_tx_hop, TBFRAME_SOF_MASK,
				       TBFRAME_EOF_MASK, NULL, NULL);
	if (!hw->rx_ring) {
		tb_ring_free(hw->tx_ring);
		hw->tx_ring = NULL;
		return -ENOMEM;
	}

	/* Rings are not running yet; tb_ring_throttling() requires that. */
	tbframe_hw_apply_throttling(hw);
	return 0;
}

static void tbframe_hw_start_rings(void *data)
{
	struct tbframe_hw *hw = data;

	tb_ring_start(hw->tx_ring);
	tb_ring_start(hw->rx_ring);
}

/*
 * Bounded TX drain into the still-programmed fabric, ahead of the BYE
 * quiesce handshake (see down_session): the backlog must leave while both
 * ends' paths exist, or it strands on the router egress (reset-only wedge).
 * Best-effort: a wedged fabric just burns the bound and stop_rings cancels
 * the rest (the loss model brackets it with link_down).
 */
static void tbframe_hw_quiesce_tx(void *data)
{
	struct tbframe_hw *hw = data;

	if (hw->tx_ring)
		tb_ring_flush(hw->tx_ring, 100);
}

static void tbframe_hw_stop_rings(void *data)
{
	struct tbframe_hw *hw = data;

	/* Cancels every enqueued frame (callback canceled=true) and returns
	 * only after all callbacks finished. */
	if (hw->rx_ring)
		tb_ring_stop(hw->rx_ring);
	if (hw->tx_ring)
		tb_ring_stop(hw->tx_ring);
}

static void tbframe_hw_free_rings(void *data)
{
	struct tbframe_hw *hw = data;

	if (hw->rx_ring) {
		tb_ring_free(hw->rx_ring);
		hw->rx_ring = NULL;
	}
	if (hw->tx_ring) {
		tb_ring_free(hw->tx_ring);
		hw->tx_ring = NULL;
	}
}

static int tbframe_hw_map_frame(void *data, struct tbframe_frame_priv *f,
				bool tx)
{
	struct tbframe_hw *hw = data;
	struct device *dev = tbframe_hw_dma_dev(hw);

	f->dma = dma_map_single(dev, f->frame.data, TBFRAME_MAX_FRAME,
				tx ? DMA_TO_DEVICE : DMA_FROM_DEVICE);
	if (dma_mapping_error(dev, f->dma))
		return -ENOMEM;
	return 0;
}

static void tbframe_hw_unmap_frame(void *data, struct tbframe_frame_priv *f,
				   bool tx)
{
	struct tbframe_hw *hw = data;

	dma_unmap_single(tbframe_hw_dma_dev(hw), f->dma, TBFRAME_MAX_FRAME,
			 tx ? DMA_TO_DEVICE : DMA_FROM_DEVICE);
}

static void tbframe_hw_tx_callback(struct tb_ring *ring,
				   struct ring_frame *rf, bool canceled)
{
	struct tbframe_frame_priv *f = container_of(rf,
						    struct tbframe_frame_priv,
						    rf);

	dma_sync_single_for_cpu(tb_ring_dma_device(ring), f->dma,
				TBFRAME_MAX_FRAME, DMA_TO_DEVICE);
	tbframe_core_tx_complete(f, canceled);
}

static void tbframe_hw_rx_callback(struct tb_ring *ring,
				   struct ring_frame *rf, bool canceled)
{
	struct tbframe_frame_priv *f = container_of(rf,
						    struct tbframe_frame_priv,
						    rf);
	bool bad = rf->flags & (RING_DESC_CRC_ERROR | RING_DESC_BUFFER_OVERRUN);

	if (!canceled)
		dma_sync_single_for_cpu(tb_ring_dma_device(ring), f->dma,
					TBFRAME_MAX_FRAME, DMA_FROM_DEVICE);
	/* The NHI writes the received frame's PDF into the descriptor's EOF
	 * field; the RX descriptor's SOF reads back 0 (tbnet reads eof too).
	 */
	tbframe_core_rx_complete(f, canceled, rf->size, rf->eof, bad);
}

static int tbframe_hw_post_rx(void *data, struct tbframe_frame_priv *f)
{
	struct tbframe_hw *hw = data;

	if (!hw->rx_ring)
		return -ESHUTDOWN;
	dma_sync_single_for_device(tbframe_hw_dma_dev(hw), f->dma,
				   TBFRAME_MAX_FRAME, DMA_FROM_DEVICE);
	f->rf.buffer_phy = f->dma;
	f->rf.callback = tbframe_hw_rx_callback;
	return tb_ring_rx(hw->rx_ring, &f->rf);
}

static int tbframe_hw_ring_tx(void *data, struct tbframe_frame_priv *f)
{
	struct tbframe_hw *hw = data;

	if (!hw->tx_ring)
		return -ESHUTDOWN;
	dma_sync_single_for_device(tbframe_hw_dma_dev(hw), f->dma,
				   f->frame.len, DMA_TO_DEVICE);
	f->rf.buffer_phy = f->dma;
	f->rf.callback = tbframe_hw_tx_callback;
	f->rf.size = f->frame.len;
	/* SOF is the frame-start marker, the frame type rides in EOF; equal
	 * sof/eof made the peer's NHI close every multi-packet frame at the
	 * first transport-packet boundary (two CRC-flagged part-frames per
	 * frame, nothing above ~252 bytes ever delivered).
	 */
	f->rf.sof = TBFRAME_PDF_SOF;
	f->rf.eof = f->frame.pdf;
	return tb_ring_tx(hw->tx_ring, &f->rf);
}

static int tbframe_hw_enable_paths(void *data, int local_hopid,
				   int remote_hopid)
{
	struct tbframe_hw *hw = data;

	return tb_xdomain_enable_paths(hw->xd, local_hopid, hw->tx_ring->hop,
				       remote_hopid, hw->rx_ring->hop);
}

static int tbframe_hw_disable_paths(void *data, int local_hopid,
				    int remote_hopid)
{
	struct tbframe_hw *hw = data;

	return tb_xdomain_disable_paths(hw->xd, local_hopid,
					hw->tx_ring ? hw->tx_ring->hop : -1,
					remote_hopid,
					hw->rx_ring ? hw->rx_ring->hop : -1);
}

static int tbframe_hw_paths_active(void *data, int local_hopid,
				   int remote_hopid)
{
#ifdef TB_XDOMAIN_HAS_PATHS_ACTIVE
	struct tbframe_hw *hw = data;

	if (!hw->tx_ring || !hw->rx_ring)
		return -EINVAL;
	return tb_xdomain_paths_active(hw->xd, local_hopid, hw->tx_ring->hop,
				       remote_hopid, hw->rx_ring->hop);
#else
	/* Unknown, not dead: without the fork core the verify never fires. */
	return -EOPNOTSUPP;
#endif
}

static int tbframe_hw_tx_snapshot(void *data,
				  struct tb_ring_snapshot *snapshot)
{
	struct tbframe_hw *hw = data;

	if (!hw->tx_ring)
		return -ESHUTDOWN;
	return tb_ring_snapshot(hw->tx_ring, snapshot);
}

static int
tbframe_hw_report_data_path_failure(void *data,
				    const struct tb_ring_snapshot *first,
				    const struct tb_ring_snapshot *last,
				    bool control_healthy)
{
	struct tbframe_hw *hw = data;

	return tb_nhi_request_runtime_recovery(hw->xd->tb->nhi, first, last,
					       control_healthy, true);
}

static void tbframe_hw_report_data_proven(void *data)
{
	struct tbframe_hw *hw = data;

	tb_nhi_runtime_data_path_proven(hw->xd->tb->nhi);
}

static int tbframe_hw_loop_control_request(struct tbframe_hw *hw,
					   const void *req, size_t req_len,
					   void *resp, size_t resp_len)
{
	u8 buf[TBFRAME_WIRE_HELLO_MSG_SIZE];
	struct tbframe_hw *sib;
	int ret = -ETIMEDOUT;

	if (req_len > sizeof(buf) || resp_len > sizeof(buf))
		return -EINVAL;
	if (!hw->tf)
		return -ENODEV;

	mutex_lock(&tbframe_loop_lock);
	sib = tbframe_hw_loop_sibling(hw);
	if (!sib) {
		/* Sibling end not probed (yet/anymore): the peer retries. */
		mutex_unlock(&tbframe_loop_lock);
		return -ETIMEDOUT;
	}

	memcpy(buf, req, req_len);
	tbframe_hw_loop_patch_route(buf, req_len, sib->xd->route);

	sib->loop_capture = resp;
	sib->loop_capture_len = resp_len;
	sib->loop_captured = false;
	tbframe_handle_packet(hw->tf, sib->xd, buf, req_len);
	if (sib->loop_captured) {
		/*
		 * The requester's protocol handler observes the response
		 * before response matching on a real wire (HELLO_ACK is
		 * applied there); mirror that ordering.
		 */
		memcpy(buf, resp, resp_len);
		tbframe_hw_loop_patch_route(buf, resp_len, hw->xd->route);
		tbframe_handle_packet(hw->tf, hw->xd, buf, resp_len);
		memcpy(resp, buf, resp_len);
		ret = 0;
	}
	sib->loop_capture = NULL;
	sib->loop_capture_len = 0;
	sib->loop_captured = false;
	mutex_unlock(&tbframe_loop_lock);

	/* A withheld ack (READY/BYE gating) is a request timeout, as on
	 * the wire; the session machinery's retry cadence handles it. */
	return ret;
}

static int tbframe_hw_control_request(void *data, const void *req,
				      size_t req_len, void *resp,
				      size_t resp_len, unsigned int timeout_ms)
{
	struct tbframe_hw *hw = data;

	if (hw->loop)
		return tbframe_hw_loop_control_request(hw, req, req_len,
						       resp, resp_len);

	return tb_xdomain_request(hw->xd, req, req_len,
				  TB_CFG_PKG_XDOMAIN_REQ, resp, resp_len,
				  TB_CFG_PKG_XDOMAIN_RESP, timeout_ms);
}

static int tbframe_hw_control_response(void *data, const void *resp,
				       size_t len)
{
	struct tbframe_hw *hw = data;

	if (hw->loop) {
		/* Caller holds tbframe_loop_lock (dispatch is synchronous
		 * inside the loop control exchange). */
		if (!hw->loop_capture || len > hw->loop_capture_len)
			return -ENOSPC;
		memcpy(hw->loop_capture, resp, len);
		hw->loop_captured = true;
		return 0;
	}

	return tb_xdomain_response(hw->xd, resp, len,
				   TB_CFG_PKG_XDOMAIN_RESP);
}

static bool tbframe_hw_reannounce(void *data)
{
#ifdef TB_XDOMAIN_HAS_REANNOUNCE
	typedef void (*reannounce_fn_t)(void);
	reannounce_fn_t fn;

	fn = symbol_get(tb_reannounce_property_dirs);
	if (!fn) {
		pr_warn_ratelimited("property re-announce unavailable in thunderbolt core\n");
		return false;
	}
	fn();
	symbol_put(tb_reannounce_property_dirs);
	return true;
#else
	pr_warn_ratelimited("property re-announce unavailable in thunderbolt headers\n");
	return false;
#endif
}

static void tbframe_hw_link_attrs(void *data, u8 *width, u8 *speed)
{
	struct tbframe_hw *hw = data;

	*width = min_t(unsigned int, hw->xd->link_width, U8_MAX);
	*speed = min_t(unsigned int, hw->xd->link_speed, U8_MAX);
}

static void tbframe_hw_peer_identity(void *data, u8 uuid[16], char *name,
				     size_t name_len)
{
	struct tbframe_hw *hw = data;

	memset(uuid, 0, 16);
	if (hw->xd->remote_uuid)
		memcpy(uuid, hw->xd->remote_uuid->b, 16);
	strscpy(name, hw->xd->device_name ? : "", name_len);
}

static bool tbframe_hw_match(void *data, const void *token)
{
	struct tbframe_hw *hw = data;

	return hw->xd == token;
}

const struct tbframe_hw_ops tbframe_hw_real_ops = {
	.alloc_out_hopid	= tbframe_hw_alloc_out_hopid,
	.release_out_hopid	= tbframe_hw_release_out_hopid,
	.alloc_in_hopid		= tbframe_hw_alloc_in_hopid,
	.release_in_hopid	= tbframe_hw_release_in_hopid,
	.alloc_rings		= tbframe_hw_alloc_rings,
	.start_rings		= tbframe_hw_start_rings,
	.quiesce_tx		= tbframe_hw_quiesce_tx,
	.stop_rings		= tbframe_hw_stop_rings,
	.free_rings		= tbframe_hw_free_rings,
	.map_frame		= tbframe_hw_map_frame,
	.unmap_frame		= tbframe_hw_unmap_frame,
	.post_rx		= tbframe_hw_post_rx,
	.ring_tx		= tbframe_hw_ring_tx,
	.enable_paths		= tbframe_hw_enable_paths,
	.disable_paths		= tbframe_hw_disable_paths,
	.paths_active		= tbframe_hw_paths_active,
	.tx_snapshot		= tbframe_hw_tx_snapshot,
	.report_data_path_failure = tbframe_hw_report_data_path_failure,
	.report_data_proven	= tbframe_hw_report_data_proven,
	.control_request	= tbframe_hw_control_request,
	.control_response	= tbframe_hw_control_response,
	.reannounce		= tbframe_hw_reannounce,
	.link_attrs		= tbframe_hw_link_attrs,
	.peer_identity		= tbframe_hw_peer_identity,
	.match			= tbframe_hw_match,
};

struct tbframe_hw *tbframe_hw_create(struct tbframe *tf, struct tb_xdomain *xd)
{
	struct tbframe_hw *hw;

	hw = kzalloc(sizeof(*hw), GFP_KERNEL);
	if (!hw)
		return NULL;
	hw->xd = tb_xdomain_get(xd);
	hw->tf = tf;
	INIT_LIST_HEAD(&hw->loop_node);

	/*
	 * An enumerated XDomain whose remote identity is our own domain is
	 * an intra-domain loop end: control plane loops in software.
	 */
	if (xd->local_uuid && xd->remote_uuid &&
	    uuid_equal(xd->local_uuid, xd->remote_uuid)) {
		hw->loop = true;
		mutex_lock(&tbframe_loop_lock);
		list_add_tail(&hw->loop_node, &tbframe_loop_hws);
		mutex_unlock(&tbframe_loop_lock);
		pr_info("tbframe0x%llx: loop control plane active\n",
			xd->route);
	}
	return hw;
}

void tbframe_hw_destroy(struct tbframe_hw *hw)
{
	if (!hw)
		return;
	if (hw->loop) {
		mutex_lock(&tbframe_loop_lock);
		list_del(&hw->loop_node);
		mutex_unlock(&tbframe_loop_lock);
	}
	tb_xdomain_put(hw->xd);
	kfree(hw);
}
