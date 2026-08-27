// SPDX-License-Identifier: GPL-2.0
/*
 * Thunderbolt driver - control channel and configuration commands
 *
 * Copyright (c) 2014 Andreas Noever <andreas.noever@gmail.com>
 * Copyright (C) 2018, Intel Corporation
 */

#include <linux/crc32.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/pci.h>
#include <linux/dmapool.h>
#include <linux/semaphore.h>
#include <linux/workqueue.h>

#include "ctl.h"

#define CREATE_TRACE_POINTS
#include "trace.h"

#define TB_CTL_RX_PKG_COUNT	10
#define TB_CTL_RETRIES		4
/*
 * Bound on the tb_ctl_free() wait for outstanding request completion work.
 * Nothing on a teardown path may wait indefinitely; on expiry the control
 * channel allocation is deliberately leaked instead (see tb_ctl_free()).
 */
#define TB_CTL_REQ_WORK_DRAIN_MS 2000
/* Windows waits this long for the independent local ring command. */
#define TB_CTL_XDOMAIN_TX_TIMEOUT_MS 25000

/**
 * struct tb_ctl - Thunderbolt control channel
 * @nhi: Pointer to the NHI structure
 * @tx: Transmit ring
 * @rx: Receive ring
 * @frame_pool: DMA pool for control messages
 * @rx_packets: Received control messages
 * @request_queue_lock: Lock protecting @request_queue
 * @request_queue: List of outstanding requests
 * @running: Is the control channel running at the moment
 * @timeout_msec: Default timeout for non-raw control messages
 * @callback: Callback called when hotplug message is received
 * @callback_data: Data passed to @callback
 * @index: Domain number. This will be output with the trace record.
 */
struct tb_ctl {
	struct tb_nhi *nhi;
	struct tb_ring *tx;
	struct tb_ring *rx;

	struct dma_pool *frame_pool;
	struct ctl_pkg *rx_packets[TB_CTL_RX_PKG_COUNT];
	struct mutex request_queue_lock;
	struct list_head request_queue;
	/* PDF 0xc has no sequence token, so local commands share one slot. */
	struct semaphore xdomain_tx_sem;
	bool xdomain_tx_occupied;
	bool running;
	/*
	 * Requests whose ->work has been accepted by system_wq and synchronous
	 * local-slot owners that have not finished. tb_cfg_request_work()
	 * dereferences req->ctl AFTER the
	 * callback (tb_cfg_request_dequeue() takes ctl->request_queue_lock),
	 * so the ctl must outlive every such item. Nothing else ties them
	 * together: the kref counts the REQUEST, the ctl has no refcount, and
	 * neither tb_ctl_stop() nor tb_ctl_free() used to flush them.
	 */
	atomic_t req_works;

	/*
	 * Ring 0 liveness, diagnostic only. When a control request times out
	 * the driver reports "no answer", which is three different faults
	 * wearing one face: the packet never left, it left and nothing came
	 * back, or replies are arriving and not matching. REG_FW_STS cannot
	 * separate them -- it reads 0x800001a1 on healthy and failing hosts
	 * alike (appmana-019 vs -021/-023, 2026-08-25) -- so the counters
	 * that distinguish "the firmware is not executing" from "we are not
	 * transmitting" have to come from the ring itself.
	 */
	atomic_t tx_done;
	atomic_t tx_canceled;
	atomic_t rx_total;
	atomic_t rx_matched;
	atomic_t rx_unmatched;
	atomic_t rx_xdomain_tx_status;
	atomic_t rx_dropped;

	/*
	 * Consecutive request timeouts with no matched reply in between, and
	 * the basis of tb_ctl_is_responsive(). Not diagnostic: the teardown
	 * path consults it before pushing config space I/O at the controller.
	 *
	 * appmana-019, 2026-08-25 17:30:26: the software CM was already
	 * failing reads ("0: timeout reading config space 1 from 0x37") when a
	 * live rmmod+modprobe ran. tb_stop() then issued its unconditional
	 * teardown writes into a controller that was not answering, and the
	 * CIO went from degraded to permanently mute -- rx_total=0 from that
	 * second onward, surviving module reloads AND warm reboots (only power
	 * removal clears it). Every one of those writes is a 250 ms..5 s
	 * timeout that accomplishes nothing when nothing is listening.
	 */
	atomic_t consec_timeouts;

	int timeout_msec;
	event_cb callback;
	void *callback_data;

	int index;
};

void tb_ctl_get_stats(const struct tb_ctl *ctl, struct tb_ctl_stats *stats)
{
	stats->tx_done = atomic_read(&ctl->tx_done);
	stats->tx_canceled = atomic_read(&ctl->tx_canceled);
	stats->rx_total = atomic_read(&ctl->rx_total);
	stats->rx_matched = atomic_read(&ctl->rx_matched);
	stats->rx_unmatched = atomic_read(&ctl->rx_unmatched);
	stats->rx_xdomain_tx_status = atomic_read(&ctl->rx_xdomain_tx_status);
	stats->rx_dropped = atomic_read(&ctl->rx_dropped);
}


#define tb_ctl_WARN(ctl, format, arg...) \
	dev_WARN(&(ctl)->nhi->pdev->dev, format, ## arg)

#define tb_ctl_err(ctl, format, arg...) \
	dev_err(&(ctl)->nhi->pdev->dev, format, ## arg)

#define tb_ctl_warn(ctl, format, arg...) \
	dev_warn(&(ctl)->nhi->pdev->dev, format, ## arg)

#define tb_ctl_info(ctl, format, arg...) \
	dev_info(&(ctl)->nhi->pdev->dev, format, ## arg)

#define tb_ctl_dbg(ctl, format, arg...) \
	dev_dbg(&(ctl)->nhi->pdev->dev, format, ## arg)

#define tb_ctl_dbg_once(ctl, format, arg...) \
	dev_dbg_once(&(ctl)->nhi->pdev->dev, format, ## arg)

/*
 * A stalled handshake retries forever, so the ring-0 diagnostic below has
 * to be rate limited or it becomes the fault.
 */
#define tb_ctl_warn_ratelimited(ctl, format, arg...) \
	dev_warn_ratelimited(&(ctl)->nhi->pdev->dev, format, ## arg)

static void tb_ctl_schedule_req_work(struct tb_cfg_request *req);
static struct tb_cfg_request_state
tb_cfg_request_read_state(struct tb_cfg_request *req);

static DECLARE_WAIT_QUEUE_HEAD(tb_cfg_request_cancel_queue);
static DECLARE_WAIT_QUEUE_HEAD(tb_cfg_request_local_queue);
/* Serializes access to request kref_get/put */
static DEFINE_MUTEX(tb_cfg_request_lock);

/**
 * tb_cfg_request_alloc() - Allocates a new config request
 *
 * This is refcounted object so when you are done with this, call
 * tb_cfg_request_put() to it.
 */
struct tb_cfg_request *tb_cfg_request_alloc(void)
{
	struct tb_cfg_request *req;

	req = kzalloc(sizeof(*req), GFP_KERNEL);
	if (!req)
		return NULL;

	kref_init(&req->kref);
	spin_lock_init(&req->state_lock);

	return req;
}

/**
 * tb_cfg_request_get() - Increase refcount of a request
 * @req: Request whose refcount is increased
 */
void tb_cfg_request_get(struct tb_cfg_request *req)
{
	mutex_lock(&tb_cfg_request_lock);
	kref_get(&req->kref);
	mutex_unlock(&tb_cfg_request_lock);
}

static void tb_cfg_request_destroy(struct kref *kref)
{
	struct tb_cfg_request *req = container_of(kref, typeof(*req), kref);

	kfree(req);
}

/**
 * tb_cfg_request_put() - Decrease refcount and possibly release the request
 * @req: Request whose refcount is decreased
 *
 * Call this function when you are done with the request. When refcount
 * goes to %0 the object is released.
 */
void tb_cfg_request_put(struct tb_cfg_request *req)
{
	mutex_lock(&tb_cfg_request_lock);
	kref_put(&req->kref, tb_cfg_request_destroy);
	mutex_unlock(&tb_cfg_request_lock);
}

static int tb_cfg_request_enqueue(struct tb_ctl *ctl,
				  struct tb_cfg_request *req)
{
	WARN_ON(test_bit(TB_CFG_REQUEST_ACTIVE, &req->flags));
	WARN_ON(req->ctl);

	mutex_lock(&ctl->request_queue_lock);
	if (!ctl->running) {
		mutex_unlock(&ctl->request_queue_lock);
		return -ENOTCONN;
	}
	req->ctl = ctl;
	list_add_tail(&req->list, &ctl->request_queue);
	set_bit(TB_CFG_REQUEST_ACTIVE, &req->flags);
	mutex_unlock(&ctl->request_queue_lock);
	return 0;
}

static void tb_cfg_request_dequeue(struct tb_cfg_request *req)
{
	struct tb_ctl *ctl = req->ctl;

	mutex_lock(&ctl->request_queue_lock);
	if (!test_bit(TB_CFG_REQUEST_ACTIVE, &req->flags)) {
		mutex_unlock(&ctl->request_queue_lock);
		return;
	}

	list_del(&req->list);
	clear_bit(TB_CFG_REQUEST_ACTIVE, &req->flags);
	if (test_bit(TB_CFG_REQUEST_CANCELED, &req->flags))
		wake_up(&tb_cfg_request_cancel_queue);
	mutex_unlock(&ctl->request_queue_lock);
}

static bool tb_cfg_request_is_active(struct tb_cfg_request *req)
{
	return test_bit(TB_CFG_REQUEST_ACTIVE, &req->flags);
}

static struct tb_cfg_request *
tb_cfg_request_find(struct tb_ctl *ctl, struct ctl_pkg *pkg)
{
	struct tb_cfg_request *req = NULL, *iter;

	mutex_lock(&pkg->ctl->request_queue_lock);
	list_for_each_entry(iter, &pkg->ctl->request_queue, list) {
		tb_cfg_request_get(iter);
		if (iter->match(iter, pkg)) {
			req = iter;
			break;
		}
		tb_cfg_request_put(iter);
	}
	mutex_unlock(&pkg->ctl->request_queue_lock);

	return req;
}

static struct tb_cfg_request *
tb_cfg_request_find_intermediate(struct tb_ctl *ctl, struct ctl_pkg *pkg,
				 enum tb_cfg_request_event *event)
{
	struct tb_cfg_request *req = NULL, *iter;

	*event = TB_CFG_REQUEST_EVENT_NONE;
	mutex_lock(&ctl->request_queue_lock);
	list_for_each_entry(iter, &ctl->request_queue, list) {
		if (!iter->intermediate)
			continue;
		if (!tb_cfg_local_slot_is_owned(tb_cfg_request_read_state(iter).local))
			continue;

		*event = iter->intermediate(iter, pkg);
		if (*event == TB_CFG_REQUEST_EVENT_NONE)
			continue;

		tb_cfg_request_get(iter);
		req = iter;
		break;
	}
	mutex_unlock(&ctl->request_queue_lock);

	return req;
}

static enum tb_cfg_request_action
tb_cfg_request_update_state(struct tb_cfg_request *req,
			    enum tb_cfg_request_event event)
{
	enum tb_cfg_request_action action;
	unsigned long flags;

	spin_lock_irqsave(&req->state_lock, flags);
	action = tb_cfg_request_state_step(&req->state, event);
	spin_unlock_irqrestore(&req->state_lock, flags);

	return action;
}

static struct tb_cfg_request_state
tb_cfg_request_read_state(struct tb_cfg_request *req)
{
	struct tb_cfg_request_state state;
	unsigned long flags;

	spin_lock_irqsave(&req->state_lock, flags);
	state = req->state;
	spin_unlock_irqrestore(&req->state_lock, flags);

	return state;
}

static void tb_cfg_request_update_tx(struct tb_cfg_request *req,
				     enum tb_cfg_tx_event event)
{
	unsigned long flags;

	spin_lock_irqsave(&req->state_lock, flags);
	tb_cfg_request_tx_step(&req->state, event);
	spin_unlock_irqrestore(&req->state_lock, flags);
}

static void tb_cfg_request_release_local_slot(struct tb_cfg_request *req)
{
	struct tb_ctl *ctl = req->ctl;

	if (!ctl || !test_and_clear_bit(TB_CFG_REQUEST_LOCAL_SLOT, &req->flags))
		return;

	WRITE_ONCE(ctl->xdomain_tx_occupied, false);
	up(&ctl->xdomain_tx_sem);
}

/* utility functions */


static int check_header(const struct ctl_pkg *pkg, u32 len,
			enum tb_cfg_pkg_type type, u64 route)
{
	struct tb_cfg_header *header = pkg->buffer;

	/* check frame, TODO: frame flags */
	if (WARN(len != pkg->frame.size,
			"wrong framesize (expected %#x, got %#x)\n",
			len, pkg->frame.size))
		return -EIO;
	if (WARN(type != pkg->frame.eof, "wrong eof (expected %#x, got %#x)\n",
			type, pkg->frame.eof))
		return -EIO;
	if (WARN(pkg->frame.sof, "wrong sof (expected 0x0, got %#x)\n",
			pkg->frame.sof))
		return -EIO;

	/* check header */
	if (WARN(header->unknown != 1 << 9,
			"header->unknown is %#x\n", header->unknown))
		return -EIO;
	if (WARN(route != tb_cfg_get_route(header),
			"wrong route (expected %llx, got %llx)",
			route, tb_cfg_get_route(header)))
		return -EIO;
	return 0;
}

static int check_config_address(struct tb_cfg_address addr,
				enum tb_cfg_space space, u32 offset,
				u32 length)
{
	if (WARN(addr.zero, "addr.zero is %#x\n", addr.zero))
		return -EIO;
	if (WARN(space != addr.space, "wrong space (expected %x, got %x\n)",
			space, addr.space))
		return -EIO;
	if (WARN(offset != addr.offset, "wrong offset (expected %x, got %x\n)",
			offset, addr.offset))
		return -EIO;
	if (WARN(length != addr.length, "wrong space (expected %x, got %x\n)",
			length, addr.length))
		return -EIO;
	/*
	 * We cannot check addr->port as it is set to the upstream port of the
	 * sender.
	 */
	return 0;
}

static struct tb_cfg_result decode_error(const struct ctl_pkg *response)
{
	struct cfg_error_pkg *pkg = response->buffer;
	struct tb_cfg_result res = { 0 };
	res.response_route = tb_cfg_get_route(&pkg->header);
	res.response_port = 0;
	res.err = check_header(response, sizeof(*pkg), TB_CFG_PKG_ERROR,
			       tb_cfg_get_route(&pkg->header));
	if (res.err)
		return res;

	res.err = 1;
	res.tb_error = pkg->error;
	res.response_port = pkg->port;
	return res;

}

static struct tb_cfg_result parse_header(const struct ctl_pkg *pkg, u32 len,
					 enum tb_cfg_pkg_type type, u64 route)
{
	struct tb_cfg_header *header = pkg->buffer;
	struct tb_cfg_result res = { 0 };

	if (pkg->frame.eof == TB_CFG_PKG_ERROR)
		return decode_error(pkg);

	res.response_port = 0; /* will be updated later for cfg_read/write */
	res.response_route = tb_cfg_get_route(header);
	res.err = check_header(pkg, len, type, route);
	return res;
}

static void tb_cfg_print_error(struct tb_ctl *ctl, enum tb_cfg_space space,
			       const struct tb_cfg_result *res)
{
	WARN_ON(res->err != 1);
	switch (res->tb_error) {
	case TB_CFG_ERROR_PORT_NOT_CONNECTED:
		/* Port is not connected. This can happen during surprise
		 * removal. Do not warn. */
		return;
	case TB_CFG_ERROR_INVALID_CONFIG_SPACE:
		/*
		 * Invalid cfg_space/offset/length combination in
		 * cfg_read/cfg_write.
		 */
		tb_ctl_dbg_once(ctl, "%llx:%x: invalid config space (%u) or offset\n",
				res->response_route, res->response_port, space);
		return;
	case TB_CFG_ERROR_NO_SUCH_PORT:
		/*
		 * - The route contains a non-existent port.
		 * - The route contains a non-PHY port (e.g. PCIe).
		 * - The port in cfg_read/cfg_write does not exist.
		 */
		tb_ctl_WARN(ctl, "CFG_ERROR(%llx:%x): Invalid port\n",
			res->response_route, res->response_port);
		return;
	case TB_CFG_ERROR_LOOP:
		tb_ctl_WARN(ctl, "CFG_ERROR(%llx:%x): Route contains a loop\n",
			res->response_route, res->response_port);
		return;
	case TB_CFG_ERROR_LOCK:
		tb_ctl_warn(ctl, "%llx:%x: downstream port is locked\n",
			    res->response_route, res->response_port);
		return;
	default:
		/* 5,6,7,9 and 11 are also valid error codes */
		tb_ctl_WARN(ctl, "CFG_ERROR(%llx:%x): Unknown error\n",
			res->response_route, res->response_port);
		return;
	}
}

static __be32 tb_crc(const void *data, size_t len)
{
	return cpu_to_be32(~crc32c(~0, data, len));
}

static void tb_ctl_pkg_free(struct ctl_pkg *pkg)
{
	if (pkg) {
		if (pkg->request)
			tb_cfg_request_put(pkg->request);
		dma_pool_free(pkg->ctl->frame_pool,
			      pkg->buffer, pkg->frame.buffer_phy);
		kfree(pkg);
	}
}

static struct ctl_pkg *tb_ctl_pkg_alloc(struct tb_ctl *ctl)
{
	struct ctl_pkg *pkg = kzalloc(sizeof(*pkg), GFP_KERNEL);
	if (!pkg)
		return NULL;
	pkg->ctl = ctl;
	pkg->buffer = dma_pool_alloc(ctl->frame_pool, GFP_KERNEL,
				     &pkg->frame.buffer_phy);
	if (!pkg->buffer) {
		kfree(pkg);
		return NULL;
	}
	return pkg;
}


/* RX/TX handling */

static void tb_ctl_tx_callback(struct tb_ring *ring, struct ring_frame *frame,
			       bool canceled)
{
	struct ctl_pkg *pkg = container_of(frame, typeof(*pkg), frame);

	if (pkg->ctl) {
		if (canceled)
			atomic_inc(&pkg->ctl->tx_canceled);
		else
			atomic_inc(&pkg->ctl->tx_done);
	}
	if (pkg->request)
		tb_cfg_request_update_tx(pkg->request, canceled ?
					 TB_CFG_TX_EVENT_CANCELED :
					 TB_CFG_TX_EVENT_CONSUMED);
	tb_ctl_pkg_free(pkg);
}

/*
 * tb_cfg_tx() - transmit a packet on the control channel
 *
 * len must be a multiple of four.
 *
 * Return: Returns 0 on success or an error code on failure.
 */
static int tb_ctl_tx(struct tb_ctl *ctl, const void *data, size_t len,
		     enum tb_cfg_pkg_type type, struct tb_cfg_request *request)
{
	int res;
	struct ctl_pkg *pkg;
	if (len % 4 != 0) { /* required for le->be conversion */
		tb_ctl_WARN(ctl, "TX: invalid size: %zu\n", len);
		return -EINVAL;
	}
	if (len > TB_FRAME_SIZE - 4) { /* checksum is 4 bytes */
		tb_ctl_WARN(ctl, "TX: packet too large: %zu/%d\n",
			    len, TB_FRAME_SIZE - 4);
		return -EINVAL;
	}
	pkg = tb_ctl_pkg_alloc(ctl);
	if (!pkg)
		return -ENOMEM;
	pkg->frame.callback = tb_ctl_tx_callback;
	pkg->frame.size = len + 4;
	pkg->frame.sof = type;
	pkg->frame.eof = type;
	if (request) {
		tb_cfg_request_update_tx(request, TB_CFG_TX_EVENT_QUEUED);
		tb_cfg_request_get(request);
		pkg->request = request;
	}

	trace_tb_tx(ctl->index, type, data, len);

	cpu_to_be32_array(pkg->buffer, data, len / 4);
	*(__be32 *) (pkg->buffer + len) = tb_crc(pkg->buffer, len);

	res = tb_ring_tx(ctl->tx, &pkg->frame);
	if (res) { /* ring is stopped */
		if (request)
			tb_cfg_request_update_tx(request,
						 TB_CFG_TX_EVENT_CANCELED);
		tb_ctl_pkg_free(pkg);
	}
	return res;
}

/*
 * tb_ctl_handle_event() - acknowledge a plug event, invoke ctl->callback
 */
static bool tb_ctl_handle_event(struct tb_ctl *ctl, enum tb_cfg_pkg_type type,
				struct ctl_pkg *pkg, size_t size)
{
	trace_tb_event(ctl->index, type, pkg->buffer, size);
	return ctl->callback(ctl->callback_data, type, pkg->buffer, size);
}

static void tb_ctl_rx_submit(struct ctl_pkg *pkg)
{
	tb_ring_rx(pkg->ctl->rx, &pkg->frame); /*
					     * We ignore failures during stop.
					     * All rx packets are referenced
					     * from ctl->rx_packets, so we do
					     * not loose them.
					     */
}

static int tb_async_error(const struct ctl_pkg *pkg)
{
	const struct cfg_error_pkg *error = pkg->buffer;

	if (pkg->frame.eof != TB_CFG_PKG_ERROR)
		return false;

	switch (error->error) {
	case TB_CFG_ERROR_LINK_ERROR:
	case TB_CFG_ERROR_HEC_ERROR_DETECTED:
	case TB_CFG_ERROR_FLOW_CONTROL_ERROR:
	case TB_CFG_ERROR_DP_BW:
	case TB_CFG_ERROR_ROP_CMPLT:
	case TB_CFG_ERROR_POP_CMPLT:
	case TB_CFG_ERROR_PCIE_WAKE:
	case TB_CFG_ERROR_DP_CON_CHANGE:
	case TB_CFG_ERROR_DPTX_DISCOVERY:
	case TB_CFG_ERROR_LINK_RECOVERY:
	case TB_CFG_ERROR_ASYM_LINK:
		return true;

	default:
		return false;
	}
}

static void tb_ctl_rx_callback(struct tb_ring *ring, struct ring_frame *frame,
			       bool canceled)
{
	struct ctl_pkg *pkg = container_of(frame, typeof(*pkg), frame);
	struct tb_cfg_request *req, *intermediate_req = NULL;
	enum tb_cfg_request_event event;
	enum tb_cfg_request_action action;
	bool xdomain_tx_status;
	__be32 crc32;

	if (canceled)
		return; /*
			 * ring is stopped, packet is referenced from
			 * ctl->rx_packets.
			 */

	/* Anything at all arriving on ring 0 counts here. */
	atomic_inc(&pkg->ctl->rx_total);

	if (frame->size < 4 || frame->size % 4 != 0) {
		atomic_inc(&pkg->ctl->rx_dropped);
		tb_ctl_err(pkg->ctl, "RX: invalid size %#x, dropping packet\n",
			   frame->size);
		goto rx;
	}

	frame->size -= 4; /* remove checksum */
	crc32 = tb_crc(pkg->buffer, frame->size);
	be32_to_cpu_array(pkg->buffer, pkg->buffer, frame->size / 4);

	switch (frame->eof) {
	case TB_CFG_PKG_READ:
	case TB_CFG_PKG_WRITE:
	case TB_CFG_PKG_ERROR:
	case TB_CFG_PKG_OVERRIDE:
	case TB_CFG_PKG_RESET:
		if (*(__be32 *)(pkg->buffer + frame->size) != crc32) {
			atomic_inc(&pkg->ctl->rx_dropped);
			tb_ctl_err(pkg->ctl,
				   "RX: checksum mismatch, dropping packet\n");
			goto rx;
		}
		if (tb_async_error(pkg)) {
			tb_ctl_handle_event(pkg->ctl, frame->eof,
					    pkg, frame->size);
			goto rx;
		}
		break;

	case TB_CFG_PKG_EVENT:
	case TB_CFG_PKG_XDOMAIN_RESP:
	case TB_CFG_PKG_XDOMAIN_REQ:
		if (*(__be32 *)(pkg->buffer + frame->size) != crc32) {
			atomic_inc(&pkg->ctl->rx_dropped);
			tb_ctl_err(pkg->ctl,
				   "RX: checksum mismatch, dropping packet\n");
			goto rx;
		}
		fallthrough;
	case TB_CFG_PKG_ICM_EVENT:
		if (tb_ctl_handle_event(pkg->ctl, frame->eof, pkg, frame->size))
			goto rx;
		break;

	default:
		break;
	}

	/*
	 * The received packet will be processed only if there is an
	 * active request and that the packet is what is expected. This
	 * prevents packets such as replies coming after timeout has
	 * triggered from messing with the active requests.
	 */
	req = tb_cfg_request_find(pkg->ctl, pkg);
	if (req) {
		struct tb_cfg_request_state state;

		state = tb_cfg_request_read_state(req);
		if (state.local != TB_CFG_LOCAL_DISABLED) {
			event = TB_CFG_REQUEST_EVENT_PEER_MATCHED;
			action = tb_cfg_request_update_state(req, event);
			if (action != TB_CFG_REQUEST_ACTION_COMPLETE) {
				tb_cfg_request_put(req);
				req = NULL;
			}
		}
	}
	xdomain_tx_status = !req && tb_ctl_is_xdomain_tx_status(frame->eof,
								 pkg->buffer,
								 frame->size);
	if (xdomain_tx_status)
		intermediate_req = tb_cfg_request_find_intermediate(pkg->ctl, pkg,
								    &event);

	if (req) {
		atomic_inc(&pkg->ctl->rx_matched);
		/*
		 * A reply we asked for came back: the controller is answering,
		 * so the run of timeouts (if any) is over. Only a MATCHED reply
		 * clears this -- unmatched ones mean ring 0 moves packets but
		 * the request/response pairing is broken, which is exactly the
		 * degraded state that must still gate teardown I/O.
		 */
		atomic_set(&pkg->ctl->consec_timeouts,
			   tb_ctl_liveness_next(atomic_read(&pkg->ctl->consec_timeouts),
						TB_CTL_EVENT_REPLY_MATCHED));
	} else if (xdomain_tx_status) {
		const struct icm_pkg_header *hdr = pkg->buffer;

		atomic_inc(&pkg->ctl->rx_xdomain_tx_status);
		if (intermediate_req) {
			action = tb_cfg_request_update_state(intermediate_req,
							     event);
			tb_cfg_request_release_local_slot(intermediate_req);
			wake_up_all(&tb_cfg_request_local_queue);
			if (action == TB_CFG_REQUEST_ACTION_FAIL) {
				intermediate_req->result.err = -EIO;
				tb_ctl_schedule_req_work(intermediate_req);
			}
		}
		if (hdr->flags & ICM_FLAGS_ERROR)
			tb_ctl_warn_ratelimited(pkg->ctl,
						"XDomain transmit completion reported an error\n");
	} else {
		atomic_inc(&pkg->ctl->rx_unmatched);
		/*
		 * A reply nobody claimed. Ring 0 is alive -- it arrived, it
		 * was well formed -- and the request that wanted it is going
		 * to time out anyway, so print what it was. eof is the packet
		 * type the matchers key on, and the first dword carries the
		 * ICM code / config-space route, which is the other half of
		 * every match predicate.
		 */
		{
			const u32 *dw = pkg->buffer;

			tb_ctl_warn_ratelimited(pkg->ctl,
						"RX: unmatched reply eof=%#x size=%u dw0=%#010x dw1=%#010x\n",
						pkg->frame.eof, frame->size,
						frame->size >= 4 ? dw[0] : 0,
						frame->size >= 8 ? dw[1] : 0);
		}
	}

	trace_tb_rx(pkg->ctl->index, frame->eof, pkg->buffer, frame->size,
		    !req && !xdomain_tx_status);

	if (req) {
		if (req->copy(req, pkg))
			tb_ctl_schedule_req_work(req);
		tb_cfg_request_put(req);
	}
	if (intermediate_req)
		tb_cfg_request_put(intermediate_req);

rx:
	tb_ctl_rx_submit(pkg);
}

/*
 * Schedule a request's completion work and account for it, so tb_ctl_free()
 * can fence the ctl against items that are still going to dereference it.
 * schedule_work() is a no-op on an already-pending item (req->work is a single
 * work_struct scheduled from the RX callback, the no-response fast path and
 * cancel), so the counter may only move when the queue actually accepted it.
 */
static void tb_ctl_schedule_req_work(struct tb_cfg_request *req)
{
	struct tb_ctl *ctl = req->ctl;

	if (ctl)
		atomic_inc(&ctl->req_works);
	if (!schedule_work(&req->work) && ctl)
		atomic_dec(&ctl->req_works);
}

static void tb_cfg_request_work(struct work_struct *work)
{
	struct tb_cfg_request *req = container_of(work, typeof(*req), work);
	struct tb_ctl *ctl = req->ctl;

	if (!test_bit(TB_CFG_REQUEST_CANCELED, &req->flags))
		req->callback(req->callback_data);

	if (!test_bit(TB_CFG_REQUEST_HOLD_LOCAL, &req->flags)) {
		tb_cfg_request_dequeue(req);
		tb_cfg_request_put(req);
	}
	/* Last touch of @ctl: after this the ctl may be freed. */
	if (ctl)
		atomic_dec(&ctl->req_works);
}

/**
 * tb_cfg_request() - Start control request not waiting for it to complete
 * @ctl: Control channel to use
 * @req: Request to start
 * @callback: Callback called when the request is completed
 * @callback_data: Data to be passed to @callback
 *
 * This queues @req on the given control channel without waiting for it
 * to complete. When the request completes @callback is called.
 */
int tb_cfg_request(struct tb_ctl *ctl, struct tb_cfg_request *req,
		   void (*callback)(void *), void *callback_data)
{
	int ret;

	req->flags &= BIT(TB_CFG_REQUEST_LOCAL_SLOT);
	if (req->state.local != TB_CFG_LOCAL_DISABLED)
		set_bit(TB_CFG_REQUEST_HOLD_LOCAL, &req->flags);
	req->callback = callback;
	req->callback_data = callback_data;
	INIT_WORK(&req->work, tb_cfg_request_work);
	INIT_LIST_HEAD(&req->list);

	tb_cfg_request_get(req);
	ret = tb_cfg_request_enqueue(ctl, req);
	if (ret)
		goto err_put;

	ret = tb_ctl_tx(ctl, req->request, req->request_size,
			req->request_type, req);
	if (ret)
		goto err_dequeue;

	if (!req->response)
		tb_ctl_schedule_req_work(req);

	return 0;

err_dequeue:
	tb_cfg_request_dequeue(req);
err_put:
	tb_cfg_request_put(req);

	return ret;
}

/**
 * tb_cfg_request_cancel() - Cancel a control request
 * @req: Request to cancel
 * @err: Error to assign to the request
 *
 * This function can be used to cancel ongoing request. It will wait
 * until the request is not active anymore.
 */
void tb_cfg_request_cancel(struct tb_cfg_request *req, int err)
{
	tb_cfg_request_update_state(req, TB_CFG_REQUEST_EVENT_CANCELED);
	set_bit(TB_CFG_REQUEST_CANCELED, &req->flags);
	tb_ctl_schedule_req_work(req);
	wait_event(tb_cfg_request_cancel_queue, !tb_cfg_request_is_active(req));
	req->result.err = err;
}

static void tb_cfg_request_complete(void *data)
{
	complete(data);
}

/**
 * tb_cfg_request_sync() - Start control request and wait until it completes
 * @ctl: Control channel to use
 * @req: Request to start
 * @timeout_msec: Timeout how long to wait @req to complete
 *
 * Starts a control request and waits until it completes. If timeout
 * triggers the request is canceled before function returns. Note the
 * caller needs to make sure only one message for given switch is active
 * at a time.
 */
struct tb_cfg_result tb_cfg_request_sync(struct tb_ctl *ctl,
					 struct tb_cfg_request *req,
					 int timeout_msec)
{
	unsigned long timeout = msecs_to_jiffies(timeout_msec);
	unsigned long local_deadline = 0;
	struct tb_cfg_request_state state;
	struct tb_cfg_result res = { 0 };
	DECLARE_COMPLETION_ONSTACK(done);
	bool hold_local;
	int ret;

	hold_local = req->state.local != TB_CFG_LOCAL_DISABLED;
	if (hold_local) {
		down(&ctl->xdomain_tx_sem);
		WARN_ON(!tb_cfg_local_slot_may_claim(READ_ONCE(ctl->xdomain_tx_occupied)));
		WRITE_ONCE(ctl->xdomain_tx_occupied, true);
		set_bit(TB_CFG_REQUEST_LOCAL_SLOT, &req->flags);
		atomic_inc(&ctl->req_works);
		local_deadline = jiffies +
			msecs_to_jiffies(TB_CTL_XDOMAIN_TX_TIMEOUT_MS);
	}

	ret = tb_cfg_request(ctl, req, tb_cfg_request_complete, &done);
	if (ret) {
		res.err = ret;
		res.tx_state = tb_cfg_request_read_state(req).tx;
		if (hold_local)
			tb_cfg_request_release_local_slot(req);
		if (hold_local)
			atomic_dec(&ctl->req_works);
		return res;
	}

	if (!wait_for_completion_timeout(&done, timeout)) {
		struct tb_ring_snapshot tx_snapshot;
		int snapshot_ret;
		int process_ret;

		tb_cfg_request_update_state(req,
					    TB_CFG_REQUEST_EVENT_PEER_TIMED_OUT);
		snapshot_ret = tb_ring_snapshot(ctl->tx, &tx_snapshot);
		state = tb_cfg_request_read_state(req);
		if (!snapshot_ret && state.tx == TB_CFG_TX_QUEUED &&
		    tb_nhi_tx_descriptor_completed(&tx_snapshot)) {
			process_ret = tb_ring_process_completions(ctl->tx);
			if (process_ret)
				tb_ctl_warn_ratelimited(ctl,
							"ring0 TX completion processing failed: %d\n",
							process_ret);
			state = tb_cfg_request_read_state(req);
			snapshot_ret = tb_ring_snapshot(ctl->tx, &tx_snapshot);
		}
		/*
		 * Say which of the three "no answer" faults this actually is.
		 * tx_done advancing but rx_total flat means we transmitted and
		 * the far side produced nothing -- the firmware really is not
		 * executing. rx_total advancing with rx_unmatched climbing
		 * means replies ARE arriving and being discarded, which is a
		 * matching/sequence bug on our side, not dead firmware.
		 * tx_done flat means we never got the packet onto the ring at
		 * all. Rate-limited: a wedged handshake retries forever.
		 */
		{
			const u32 *rdw = req->request;

			tb_ctl_warn_ratelimited(ctl,
						"request timed out: send eof=%#x want eof=%#x route=%#llx req_dw0=%#010x size=%zu\n",
						req->request_type,
						req->response_type,
						tb_cfg_request_route(req->request_type,
								     req->request,
								     req->request_size),
						req->request_size >= 4 ? rdw[0] : 0,
						req->request_size);
		}
		tb_ctl_warn_ratelimited(ctl,
					"request timed out after %d ms; tx_state=%u local_state=%u peer_state=%u ring0 tx_done=%d tx_canceled=%d rx_total=%d rx_matched=%d rx_unmatched=%d rx_xdomain_tx_status=%d rx_dropped=%d\n",
					timeout_msec,
					state.tx, state.local, state.peer,
					atomic_read(&ctl->tx_done),
					atomic_read(&ctl->tx_canceled),
					atomic_read(&ctl->rx_total),
					atomic_read(&ctl->rx_matched),
					atomic_read(&ctl->rx_unmatched),
					atomic_read(&ctl->rx_xdomain_tx_status),
					atomic_read(&ctl->rx_dropped));
		if (!snapshot_ret)
			tb_ctl_warn_ratelimited(ctl,
						"ring0 TX snapshot: running=%u size=%u sw_head=%u sw_tail=%u queued=%u in_flight=%u tail_attr=%#010x tail_complete=%u index=%#010x hw_prod=%u hw_cons=%u indices_valid=%u full=%u options=%#010x\n",
					tx_snapshot.running, tx_snapshot.size,
					tx_snapshot.sw_head, tx_snapshot.sw_tail,
					tx_snapshot.queued, tx_snapshot.in_flight,
					tx_snapshot.tail_attributes,
					tb_nhi_tx_descriptor_completed(&tx_snapshot),
					tx_snapshot.index_raw,
					tx_snapshot.hw_producer,
					tx_snapshot.hw_consumer,
					tx_snapshot.indices_valid,
					tx_snapshot.full, tx_snapshot.options);
		else
			tb_ctl_warn_ratelimited(ctl,
						"ring0 TX snapshot unavailable: %d\n",
				snapshot_ret);
		if (state.local == TB_CFG_LOCAL_ACCEPTED)
			tb_ctl_warn_ratelimited(ctl,
						"peer response timed out after local XDomain transmit acceptance\n");
		atomic_inc(&ctl->consec_timeouts);
		if (hold_local) {
			set_bit(TB_CFG_REQUEST_CANCELED, &req->flags);
			req->result.err = -ETIMEDOUT;
		} else {
			tb_cfg_request_cancel(req, -ETIMEDOUT);
		}
	}

	flush_work(&req->work);
	if (hold_local) {
		unsigned long remaining = time_before(jiffies, local_deadline) ?
			local_deadline - jiffies : 0;

		state = tb_cfg_request_read_state(req);
		if (state.local == TB_CFG_LOCAL_WAITING && remaining) {
			wait_event_timeout(tb_cfg_request_local_queue,
					   tb_cfg_request_read_state(req).local !=
						TB_CFG_LOCAL_WAITING,
					   remaining);
		}
		state = tb_cfg_request_read_state(req);
		if (state.local == TB_CFG_LOCAL_WAITING) {
			tb_cfg_request_update_state(req,
						    TB_CFG_REQUEST_EVENT_LOCAL_TIMED_OUT);
			tb_ctl_warn_ratelimited(ctl,
						"local XDomain transmit completion timed out after %d ms\n",
						TB_CTL_XDOMAIN_TX_TIMEOUT_MS);
		}

		clear_bit(TB_CFG_REQUEST_HOLD_LOCAL, &req->flags);
		tb_cfg_request_dequeue(req);
		tb_cfg_request_release_local_slot(req);
		tb_cfg_request_put(req);
		/* Last touch of @ctl: tb_ctl_free() may proceed after this. */
		atomic_dec(&ctl->req_works);
	}

	res = req->result;
	res.tx_state = tb_cfg_request_read_state(req).tx;
	return res;
}

/**
 * tb_ctl_is_responsive() - Is the control channel still answering?
 * @ctl: Control channel
 *
 * Reports whether ring 0 has produced a MATCHED reply recently enough to be
 * worth talking to. Intended for paths that are about to issue config space
 * I/O they do not strictly need -- above all teardown, which otherwise fires
 * a burst of writes at a controller that has stopped answering and can push a
 * merely degraded CIO into a permanent hang (appmana-019, see consec_timeouts).
 *
 * This is deliberately one-directional: it may report a live channel as dead
 * for a few requests after a transient stall, which only costs some skipped
 * cleanup on a controller that is being torn down anyway. It must never report
 * a dead channel as live, so the counter is cleared ONLY by a matched reply.
 *
 * Return: %true when the channel is worth using, %false when it has failed
 * %TB_CTL_DEAD_TIMEOUTS requests in a row without a matched reply.
 */
bool tb_ctl_is_responsive(struct tb_ctl *ctl)
{
	if (!ctl)
		return false;

	return !tb_ctl_timeouts_indicate_dead(atomic_read(&ctl->consec_timeouts));
}

/* public interface, alloc/start/stop/free */

/**
 * tb_ctl_alloc() - allocate a control channel
 * @nhi: Pointer to NHI
 * @index: Domain number
 * @timeout_msec: Default timeout used with non-raw control messages
 * @cb: Callback called for plug events
 * @cb_data: Data passed to @cb
 *
 * cb will be invoked once for every hot plug event.
 *
 * Return: Returns a pointer on success or NULL on failure.
 */
struct tb_ctl *tb_ctl_alloc(struct tb_nhi *nhi, int index, int timeout_msec,
			    event_cb cb, void *cb_data)
{
	int i;
	struct tb_ctl *ctl = kzalloc(sizeof(*ctl), GFP_KERNEL);
	if (!ctl)
		return NULL;

	ctl->nhi = nhi;
	ctl->index = index;
	ctl->timeout_msec = timeout_msec;
	ctl->callback = cb;
	ctl->callback_data = cb_data;

	mutex_init(&ctl->request_queue_lock);
	sema_init(&ctl->xdomain_tx_sem, 1);
	INIT_LIST_HEAD(&ctl->request_queue);
	ctl->frame_pool = dma_pool_create("thunderbolt_ctl", &nhi->pdev->dev,
					 TB_FRAME_SIZE, 4, 0);
	if (!ctl->frame_pool)
		goto err;

	ctl->tx = tb_ring_alloc_tx(nhi, 0, 10, RING_FLAG_NO_SUSPEND);
	if (!ctl->tx)
		goto err;

	ctl->rx = tb_ring_alloc_rx(nhi, 0, 10, RING_FLAG_NO_SUSPEND, 0, 0xffff,
				   0xffff, NULL, NULL);
	if (!ctl->rx)
		goto err;

	for (i = 0; i < TB_CTL_RX_PKG_COUNT; i++) {
		ctl->rx_packets[i] = tb_ctl_pkg_alloc(ctl);
		if (!ctl->rx_packets[i])
			goto err;
		ctl->rx_packets[i]->frame.callback = tb_ctl_rx_callback;
	}

	tb_ctl_dbg(ctl, "control channel created\n");
	return ctl;
err:
	tb_ctl_free(ctl);
	return NULL;
}

/**
 * tb_ctl_free() - free a control channel
 * @ctl: Control channel to free
 *
 * Must be called after tb_ctl_stop.
 *
 * Must NOT be called from ctl->callback.
 */
void tb_ctl_free(struct tb_ctl *ctl)
{
	unsigned int waited = 0;
	int i;

	if (!ctl)
		return;

	/*
	 * Bounded fence against completion work that still dereferences this
	 * ctl. tb_cfg_request_work() calls tb_cfg_request_dequeue(), which
	 * takes ctl->request_queue_lock, AFTER the callback -- and those work
	 * items run on system_wq with no reference on the ctl or the domain,
	 * so kfree(ctl) below could race them. The reachable case on the
	 * teardown path is a peer that is still sending XDomain requests at
	 * us: tb_xdp_handle_request() -> tb_xdomain_response() ->
	 * tb_cfg_request() with a NULL response schedules the work
	 * immediately.
	 *
	 * Not done in tb_ctl_stop(): that runs under tb->lock, and one of the
	 * callbacks (icm_usb4_switch_nvm_auth_complete()) takes tb->lock, so
	 * waiting there would be the very lock inversion this series exists to
	 * remove. Here the lock is not held.
	 *
	 * Bounded with a defined failure action, per the no-unbounded-waits
	 * rule: on expiry, warn and proceed. Leaking the ctl allocation is
	 * strictly better than never returning from a module unload or a
	 * shutdown.
	 */
	while (atomic_read(&ctl->req_works)) {
		if (waited >= TB_CTL_REQ_WORK_DRAIN_MS) {
			tb_ctl_WARN(ctl,
				    "%d request work items still outstanding after %u ms; leaking the control channel rather than blocking teardown\n",
				    atomic_read(&ctl->req_works), waited);
			return;
		}
		msleep(20);
		waited += 20;
	}

	if (ctl->rx)
		tb_ring_free(ctl->rx);
	if (ctl->tx)
		tb_ring_free(ctl->tx);

	/* free RX packets */
	for (i = 0; i < TB_CTL_RX_PKG_COUNT; i++)
		tb_ctl_pkg_free(ctl->rx_packets[i]);


	dma_pool_destroy(ctl->frame_pool);
	kfree(ctl);
}

/**
 * tb_ctl_start() - start/resume the control channel
 * @ctl: Control channel to start
 */
void tb_ctl_start(struct tb_ctl *ctl)
{
	int i;
	tb_ctl_dbg(ctl, "control channel starting...\n");
	tb_ring_start(ctl->tx); /* is used to ack hotplug packets, start first */
	tb_ring_start(ctl->rx);
	for (i = 0; i < TB_CTL_RX_PKG_COUNT; i++)
		tb_ctl_rx_submit(ctl->rx_packets[i]);

	ctl->running = true;
}

/**
 * tb_ctl_stop() - pause the control channel
 * @ctl: Control channel to stop
 *
 * All invocations of ctl->callback will have finished after this method
 * returns.
 *
 * Must NOT be called from ctl->callback.
 */
void tb_ctl_stop(struct tb_ctl *ctl)
{
	mutex_lock(&ctl->request_queue_lock);
	ctl->running = false;
	mutex_unlock(&ctl->request_queue_lock);

	tb_ring_stop(ctl->rx);
	tb_ring_stop(ctl->tx);

	/*
	 * Retire the dangling requests properly instead of re-heading the
	 * list. INIT_LIST_HEAD() left every dangling request LINKED to its
	 * stale neighbours with TB_CFG_REQUEST_ACTIVE still set, so the next
	 * tb_cfg_request_dequeue() for one of them did list_del() through
	 * freed neighbour pointers -- list corruption on a teardown path,
	 * which is the worst place to have it. The WARN below documents that
	 * this state is expected to occur, so it has to be handled, not just
	 * reported. Also wake anyone inside tb_cfg_request_cancel(), which
	 * waits on TB_CFG_REQUEST_ACTIVE clearing and would otherwise wait
	 * forever for a request the ctl has just abandoned.
	 */
	mutex_lock(&ctl->request_queue_lock);
	if (!list_empty(&ctl->request_queue)) {
		struct tb_cfg_request *req, *n;

		tb_ctl_WARN(ctl, "dangling request in request_queue\n");
		list_for_each_entry_safe(req, n, &ctl->request_queue, list) {
			list_del_init(&req->list);
			clear_bit(TB_CFG_REQUEST_ACTIVE, &req->flags);
		}
		wake_up(&tb_cfg_request_cancel_queue);
	}
	mutex_unlock(&ctl->request_queue_lock);
	tb_ctl_dbg(ctl, "control channel stopped\n");
}

/* public interface, commands */

/**
 * tb_cfg_ack_notification() - Ack notification
 * @ctl: Control channel to use
 * @route: Router that originated the event
 * @error: Pointer to the notification package
 *
 * Call this as response for non-plug notification to ack it. Returns
 * %0 on success or an error code on failure.
 */
int tb_cfg_ack_notification(struct tb_ctl *ctl, u64 route,
			    const struct cfg_error_pkg *error)
{
	struct cfg_ack_pkg pkg = {
		.header = tb_cfg_make_header(route),
	};
	const char *name;

	switch (error->error) {
	case TB_CFG_ERROR_LINK_ERROR:
		name = "link error";
		break;
	case TB_CFG_ERROR_HEC_ERROR_DETECTED:
		name = "HEC error";
		break;
	case TB_CFG_ERROR_FLOW_CONTROL_ERROR:
		name = "flow control error";
		break;
	case TB_CFG_ERROR_DP_BW:
		name = "DP_BW";
		break;
	case TB_CFG_ERROR_ROP_CMPLT:
		name = "router operation completion";
		break;
	case TB_CFG_ERROR_POP_CMPLT:
		name = "port operation completion";
		break;
	case TB_CFG_ERROR_PCIE_WAKE:
		name = "PCIe wake";
		break;
	case TB_CFG_ERROR_DP_CON_CHANGE:
		name = "DP connector change";
		break;
	case TB_CFG_ERROR_DPTX_DISCOVERY:
		name = "DPTX discovery";
		break;
	case TB_CFG_ERROR_LINK_RECOVERY:
		name = "link recovery";
		break;
	case TB_CFG_ERROR_ASYM_LINK:
		name = "asymmetric link";
		break;
	default:
		name = "unknown";
		break;
	}

	tb_ctl_dbg(ctl, "acking %s (%#x) notification on %llx\n", name,
		   error->error, route);

	return tb_ctl_tx(ctl, &pkg, sizeof(pkg), TB_CFG_PKG_NOTIFY_ACK, NULL);
}

/**
 * tb_cfg_ack_plug() - Ack hot plug/unplug event
 * @ctl: Control channel to use
 * @route: Router that originated the event
 * @port: Port where the hot plug/unplug happened
 * @unplug: Ack hot plug or unplug
 *
 * Call this as response for hot plug/unplug event to ack it.
 * Returns %0 on success or an error code on failure.
 */
int tb_cfg_ack_plug(struct tb_ctl *ctl, u64 route, u32 port, bool unplug)
{
	struct cfg_error_pkg pkg = {
		.header = tb_cfg_make_header(route),
		.port = port,
		.error = TB_CFG_ERROR_ACK_PLUG_EVENT,
		.pg = unplug ? TB_CFG_ERROR_PG_HOT_UNPLUG
			     : TB_CFG_ERROR_PG_HOT_PLUG,
	};
	tb_ctl_dbg(ctl, "acking hot %splug event on %llx:%u\n",
		   unplug ? "un" : "", route, port);
	return tb_ctl_tx(ctl, &pkg, sizeof(pkg), TB_CFG_PKG_ERROR, NULL);
}

static bool tb_cfg_match(const struct tb_cfg_request *req,
			 const struct ctl_pkg *pkg)
{
	u64 route = tb_cfg_get_route(pkg->buffer) & ~BIT_ULL(63);

	if (pkg->frame.eof == TB_CFG_PKG_ERROR)
		return true;

	if (pkg->frame.eof != req->response_type)
		return false;
	if (route != tb_cfg_get_route(req->request))
		return false;
	if (pkg->frame.size != req->response_size)
		return false;

	if (pkg->frame.eof == TB_CFG_PKG_READ ||
	    pkg->frame.eof == TB_CFG_PKG_WRITE) {
		const struct cfg_read_pkg *req_hdr = req->request;
		const struct cfg_read_pkg *res_hdr = pkg->buffer;

		if (req_hdr->addr.seq != res_hdr->addr.seq)
			return false;
	}

	return true;
}

static bool tb_cfg_copy(struct tb_cfg_request *req, const struct ctl_pkg *pkg)
{
	struct tb_cfg_result res;

	/* Now make sure it is in expected format */
	res = parse_header(pkg, req->response_size, req->response_type,
			   tb_cfg_get_route(req->request));
	if (!res.err)
		memcpy(req->response, pkg->buffer, req->response_size);

	req->result = res;

	/* Always complete when first response is received */
	return true;
}

/**
 * tb_cfg_reset() - send a reset packet and wait for a response
 * @ctl: Control channel pointer
 * @route: Router string for the router to send reset
 *
 * If the switch at route is incorrectly configured then we will not receive a
 * reply (even though the switch will reset). The caller should check for
 * -ETIMEDOUT and attempt to reconfigure the switch.
 */
struct tb_cfg_result tb_cfg_reset(struct tb_ctl *ctl, u64 route)
{
	struct cfg_reset_pkg request = { .header = tb_cfg_make_header(route) };
	struct tb_cfg_result res = { 0 };
	struct tb_cfg_header reply;
	struct tb_cfg_request *req;

	req = tb_cfg_request_alloc();
	if (!req) {
		res.err = -ENOMEM;
		return res;
	}

	req->match = tb_cfg_match;
	req->copy = tb_cfg_copy;
	req->request = &request;
	req->request_size = sizeof(request);
	req->request_type = TB_CFG_PKG_RESET;
	req->response = &reply;
	req->response_size = sizeof(reply);
	req->response_type = TB_CFG_PKG_RESET;

	res = tb_cfg_request_sync(ctl, req, ctl->timeout_msec);

	tb_cfg_request_put(req);

	return res;
}

/**
 * tb_cfg_read_raw() - read from config space into buffer
 * @ctl: Pointer to the control channel
 * @buffer: Buffer where the data is read
 * @route: Route string of the router
 * @port: Port number when reading from %TB_CFG_PORT, %0 otherwise
 * @space: Config space selector
 * @offset: Dword word offset of the register to start reading
 * @length: Number of dwords to read
 * @timeout_msec: Timeout in ms how long to wait for the response
 *
 * Reads from router config space without translating the possible error.
 */
static struct tb_cfg_result
tb_cfg_read_raw_retries(struct tb_ctl *ctl, void *buffer, u64 route, u32 port,
			enum tb_cfg_space space, u32 offset, u32 length,
			int timeout_msec, unsigned int max_retries)
{
	struct tb_cfg_result res = { 0 };
	struct cfg_read_pkg request = {
		.header = tb_cfg_make_header(route),
		.addr = {
			.port = port,
			.space = space,
			.offset = offset,
			.length = length,
		},
	};
	struct cfg_write_pkg reply;
	int retries = 0;

	while (retries < max_retries) {
		struct tb_cfg_request *req;

		req = tb_cfg_request_alloc();
		if (!req) {
			res.err = -ENOMEM;
			return res;
		}

		request.addr.seq = retries++;

		req->match = tb_cfg_match;
		req->copy = tb_cfg_copy;
		req->request = &request;
		req->request_size = sizeof(request);
		req->request_type = TB_CFG_PKG_READ;
		req->response = &reply;
		req->response_size = 12 + 4 * length;
		req->response_type = TB_CFG_PKG_READ;

		res = tb_cfg_request_sync(ctl, req, timeout_msec);

		tb_cfg_request_put(req);

		if (res.err != -ETIMEDOUT)
			break;

		/* Wait a bit (arbitrary time) until we send a retry */
		usleep_range(10, 100);
	}

	if (res.err)
		return res;

	res.response_port = reply.addr.port;
	res.err = check_config_address(reply.addr, space, offset, length);
	if (!res.err)
		memcpy(buffer, &reply.data, 4 * length);
	return res;
}

struct tb_cfg_result
tb_cfg_read_raw(struct tb_ctl *ctl, void *buffer, u64 route, u32 port,
		enum tb_cfg_space space, u32 offset, u32 length, int timeout_msec)
{
	return tb_cfg_read_raw_retries(ctl, buffer, route, port, space, offset,
				       length, timeout_msec, TB_CTL_RETRIES);
}

/**
 * tb_cfg_write_raw() - write from buffer into config space
 * @ctl: Pointer to the control channel
 * @buffer: Data to write
 * @route: Route string of the router
 * @port: Port number when writing to %TB_CFG_PORT, %0 otherwise
 * @space: Config space selector
 * @offset: Dword word offset of the register to start writing
 * @length: Number of dwords to write
 * @timeout_msec: Timeout in ms how long to wait for the response
 *
 * Writes to router config space without translating the possible error.
 */
static struct tb_cfg_result
tb_cfg_write_raw_retries(struct tb_ctl *ctl, const void *buffer, u64 route,
			 u32 port, enum tb_cfg_space space, u32 offset,
			 u32 length, int timeout_msec, unsigned int max_retries)
{
	struct tb_cfg_result res = { 0 };
	struct cfg_write_pkg request = {
		.header = tb_cfg_make_header(route),
		.addr = {
			.port = port,
			.space = space,
			.offset = offset,
			.length = length,
		},
	};
	struct cfg_read_pkg reply;
	int retries = 0;

	memcpy(&request.data, buffer, length * 4);

	while (retries < max_retries) {
		struct tb_cfg_request *req;

		req = tb_cfg_request_alloc();
		if (!req) {
			res.err = -ENOMEM;
			return res;
		}

		request.addr.seq = retries++;

		req->match = tb_cfg_match;
		req->copy = tb_cfg_copy;
		req->request = &request;
		req->request_size = 12 + 4 * length;
		req->request_type = TB_CFG_PKG_WRITE;
		req->response = &reply;
		req->response_size = sizeof(reply);
		req->response_type = TB_CFG_PKG_WRITE;

		res = tb_cfg_request_sync(ctl, req, timeout_msec);

		tb_cfg_request_put(req);

		if (res.err != -ETIMEDOUT)
			break;

		/* Wait a bit (arbitrary time) until we send a retry */
		usleep_range(10, 100);
	}

	if (res.err)
		return res;

	res.response_port = reply.addr.port;
	res.err = check_config_address(reply.addr, space, offset, length);
	return res;
}

struct tb_cfg_result tb_cfg_write_raw(struct tb_ctl *ctl, const void *buffer,
		u64 route, u32 port, enum tb_cfg_space space,
		u32 offset, u32 length, int timeout_msec)
{
	return tb_cfg_write_raw_retries(ctl, buffer, route, port, space, offset,
					length, timeout_msec, TB_CTL_RETRIES);
}

struct tb_cfg_result
tb_cfg_write_raw_once(struct tb_ctl *ctl, const void *buffer, u64 route,
			      u32 port, enum tb_cfg_space space, u32 offset,
			      u32 length, int timeout_msec)
{
	return tb_cfg_write_raw_retries(ctl, buffer, route, port, space, offset,
					length, timeout_msec, 1);
}

static int tb_cfg_get_error(struct tb_ctl *ctl, enum tb_cfg_space space,
			    const struct tb_cfg_result *res)
{
	/*
	 * For unimplemented ports access to port config space may return
	 * TB_CFG_ERROR_INVALID_CONFIG_SPACE (alternatively their type is
	 * set to TB_TYPE_INACTIVE). In the former case return -ENODEV so
	 * that the caller can mark the port as disabled.
	 */
	if (space == TB_CFG_PORT &&
	    res->tb_error == TB_CFG_ERROR_INVALID_CONFIG_SPACE)
		return -ENODEV;

	tb_cfg_print_error(ctl, space, res);

	if (res->tb_error == TB_CFG_ERROR_LOCK)
		return -EACCES;
	if (res->tb_error == TB_CFG_ERROR_PORT_NOT_CONNECTED)
		return -ENOTCONN;

	return -EIO;
}

int tb_cfg_read(struct tb_ctl *ctl, void *buffer, u64 route, u32 port,
		enum tb_cfg_space space, u32 offset, u32 length)
{
	struct tb_cfg_result res = tb_cfg_read_raw(ctl, buffer, route, port,
			space, offset, length, ctl->timeout_msec);
	switch (res.err) {
	case 0:
		/* Success */
		break;

	case 1:
		/* Thunderbolt error, tb_error holds the actual number */
		return tb_cfg_get_error(ctl, space, &res);

	case -ETIMEDOUT:
		tb_ctl_warn(ctl, "%llx: timeout reading config space %u from %#x\n",
			    route, space, offset);
		break;

	default:
		WARN(1, "tb_cfg_read: %d\n", res.err);
		break;
	}
	return res.err;
}

int tb_cfg_write(struct tb_ctl *ctl, const void *buffer, u64 route, u32 port,
		 enum tb_cfg_space space, u32 offset, u32 length)
{
	struct tb_cfg_result res = tb_cfg_write_raw(ctl, buffer, route, port,
			space, offset, length, ctl->timeout_msec);
	switch (res.err) {
	case 0:
		/* Success */
		break;

	case 1:
		/* Thunderbolt error, tb_error holds the actual number */
		return tb_cfg_get_error(ctl, space, &res);

	case -ETIMEDOUT:
		tb_ctl_warn(ctl, "%llx: timeout writing config space %u to %#x\n",
			    route, space, offset);
		break;

	default:
		WARN(1, "tb_cfg_write: %d\n", res.err);
		break;
	}
	return res.err;
}

/**
 * tb_cfg_get_upstream_port() - get upstream port number of switch at route
 * @ctl: Pointer to the control channel
 * @route: Route string of the router
 *
 * Reads the first dword from the switches TB_CFG_SWITCH config area and
 * returns the port number from which the reply originated.
 *
 * Return: Returns the upstream port number on success or an error code on
 * failure.
 */
int tb_cfg_get_upstream_port(struct tb_ctl *ctl, u64 route)
{
	u32 dummy;
	struct tb_cfg_result res = tb_cfg_read_raw(ctl, &dummy, route, 0,
						   TB_CFG_SWITCH, 0, 1,
						   ctl->timeout_msec);
	if (res.err == 1)
		return -EIO;
	if (res.err)
		return res.err;
	return res.response_port;
}
