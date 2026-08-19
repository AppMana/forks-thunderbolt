// SPDX-License-Identifier: GPL-2.0
/*
 * tbframe hardware backend: the only file that touches tb_ring/tb_xdomain.
 * Everything above it (core.c) sees struct tbframe_hw_ops.
 */

#define pr_fmt(fmt) "tbframe: " fmt

#include <linux/dma-mapping.h>
#include <linux/module.h>
#include <linux/slab.h>

#include "tbframe_priv.h"

struct tbframe_hw {
	struct tb_xdomain	*xd;
	struct tb_ring		*tx_ring;
	struct tb_ring		*rx_ring;
};

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
	return 0;
}

static void tbframe_hw_start_rings(void *data)
{
	struct tbframe_hw *hw = data;

	tb_ring_start(hw->tx_ring);
	tb_ring_start(hw->rx_ring);
}

static void tbframe_hw_stop_rings(void *data)
{
	struct tbframe_hw *hw = data;

	/*
	 * Bounded TX drain before the stop: give the fabric a chance to
	 * consume what is already posted so tb_ring_stop() does not truncate
	 * a frame mid-DMA into the router, stranding packets on the egress
	 * path that the following hop-entry deactivation then cannot flush.
	 * Best-effort and bounded: a wedged egress just burns the timeout,
	 * and the stop cancels whatever remains (the loss model brackets it
	 * with link_down). tb_ring_flush() only waits, never blocks the stop.
	 */
	if (hw->tx_ring)
		tb_ring_flush(hw->tx_ring, 100);

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

static int tbframe_hw_control_request(void *data, const void *req,
				      size_t req_len, void *resp,
				      size_t resp_len, unsigned int timeout_ms)
{
	struct tbframe_hw *hw = data;

	return tb_xdomain_request(hw->xd, req, req_len,
				  TB_CFG_PKG_XDOMAIN_REQ, resp, resp_len,
				  TB_CFG_PKG_XDOMAIN_RESP, timeout_ms);
}

static int tbframe_hw_control_response(void *data, const void *resp,
				       size_t len)
{
	struct tbframe_hw *hw = data;

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
	.stop_rings		= tbframe_hw_stop_rings,
	.free_rings		= tbframe_hw_free_rings,
	.map_frame		= tbframe_hw_map_frame,
	.unmap_frame		= tbframe_hw_unmap_frame,
	.post_rx		= tbframe_hw_post_rx,
	.ring_tx		= tbframe_hw_ring_tx,
	.enable_paths		= tbframe_hw_enable_paths,
	.disable_paths		= tbframe_hw_disable_paths,
	.paths_active		= tbframe_hw_paths_active,
	.control_request	= tbframe_hw_control_request,
	.control_response	= tbframe_hw_control_response,
	.reannounce		= tbframe_hw_reannounce,
	.link_attrs		= tbframe_hw_link_attrs,
	.peer_identity		= tbframe_hw_peer_identity,
	.match			= tbframe_hw_match,
};

struct tbframe_hw *tbframe_hw_create(struct tb_xdomain *xd)
{
	struct tbframe_hw *hw;

	hw = kzalloc(sizeof(*hw), GFP_KERNEL);
	if (!hw)
		return NULL;
	hw->xd = tb_xdomain_get(xd);
	return hw;
}

void tbframe_hw_destroy(struct tbframe_hw *hw)
{
	if (!hw)
		return;
	tb_xdomain_put(hw->xd);
	kfree(hw);
}
