// SPDX-License-Identifier: GPL-2.0

#include <linux/errno.h>
#include <linux/dma-mapping.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/thunderbolt.h>

#include "../proto/native_data.h"
#include "tbv.h"

#define TBV_NATIVE_RING_SIZE 1024

/*
 * Native ring and TX-pipeline tuning. New peers negotiate away the old
 * PATH_CREDIT protocol when both ends use the NHI hardware E2E credit path.
 * The software window remains as a rolling-upgrade/non-E2E fallback.
 */
static uint native_ring_size = TBV_NATIVE_RING_SIZE;
module_param(native_ring_size, uint, 0444);
MODULE_PARM_DESC(native_ring_size,
		 "Native TX/RX ring size (power of two)");

static uint data_tx_max_inflight = 32;
module_param(data_tx_max_inflight, uint, 0644);
MODULE_PARM_DESC(data_tx_max_inflight,
		 "Max native data frames posted to the TX ring before draining completions");

/* Apple-originated bursts can exhaust a 256-entry RX ring before credits
 * recycle. 1024 entries passed checked Mac-to-Linux UC bursts beyond one full
 * ring while keeping per-direction buffer cost modest.
 */
#define TBV_APPLE_RING_SIZE 16384
#define TBV_DATA_FRAME_SIZE SZ_4K
#define TBV_CONTROL_FRAME_SIZE 256
#define TBV_CONTROL_QUEUE_MULTIPLIER 4
#define TBV_DATA_CREDIT_CONTROL_RESERVE 256
#define TBV_DATA_TX_MAX_INFLIGHT 32
/*
 * TX completion is interrupt-driven (NHI MSI-X -> ring_work -> frame->callback)
 * and measured to drain ~100% of completions on its own; the supplemental
 * poller is a straggler/stall-warning watchdog, not the completion path. A/B on
 * 020<->009 (single x1 20G lane, RC ib_write_bw): busy 1ms re-arm vs 50ms
 * watchdog vs poller-off were throughput-identical within run variance
 * (~16.4 uni / ~30.5 duplex Gb/s), so the per-millisecond re-arm is wasted
 * wakeups. Default to a watchdog cadence; 0 disables the poller entirely.
 */
#define TBV_TX_POLL_DELAY_MS 50
#define TBV_RX_SUPP_POLL_DELAY_MS 1
#define TBV_RX_SUPP_POLL_WINDOW_MS 16
/*
 * Raw-stream zcopy serializes each DMA path, so several QPs sharing a rail can
 * briefly queue a full TX-depth worth of packetized WRs behind one active
 * stream. Keep enough metadata headroom for qps=8/TX-depth=16/1 MiB WRITE and
 * qps=4/TX-depth=128/64 KiB without reporting a false SQ-full error.
 */
#define TBV_DATA_QUEUE_MULTIPLIER 64
#define TBV_DATA_PACKET_POOL_LIMIT 1024

typedef int (*tbv_ring_throttling_fn)(struct tb_ring *ring,
				      unsigned int interval_nsec);

extern int tb_ring_throttling(struct tb_ring *ring,
			      unsigned int interval_nsec);

static uint nhi_interrupt_throttle_ns;
module_param(nhi_interrupt_throttle_ns, uint, 0644);
MODULE_PARM_DESC(nhi_interrupt_throttle_ns,
		 "NHI interrupt throttling interval for TBV data rings in ns; 0 disables ring throttling");

/*
 * Supplemental TX-completion poller cadence. The native TX ring is already
 * interrupt-driven (tb_ring_alloc_tx with no start_poll -> NHI MSI-X ->
 * ring_work -> frame->callback), which empirically drains ~99.5% of
 * completions. This delayed-work poller is a backstop that drains the small
 * tail left after inflight quiesces (no further interrupt until the next post).
 *   >0  : re-arm interval in ms while inflight remains (1 = legacy busy-poll).
 *    0  : disable the poller entirely -> rely purely on interrupt completion.
 * A large value (e.g. 50) keeps a watchdog drain + the tx-stall warning while
 * removing the per-millisecond busy re-arm.
 */
static uint tx_poll_delay_ms = TBV_TX_POLL_DELAY_MS;
module_param(tx_poll_delay_ms, uint, 0644);
MODULE_PARM_DESC(tx_poll_delay_ms,
		 "Supplemental TX-completion poll re-arm interval in ms (0 disables; interrupt path still completes)");

static tbv_ring_throttling_fn tbv_ring_throttling;

void tbv_path_init_optional_symbols(void)
{
	tbv_ring_throttling = symbol_get(tb_ring_throttling);
	if (tbv_ring_throttling)
		pr_info("using optional tb_ring_throttling() helper\n");
	else
		pr_info("optional tb_ring_throttling() helper unavailable; using stock NHI interrupt throttling\n");
}

void tbv_path_exit_optional_symbols(void)
{
	if (!tbv_ring_throttling)
		return;

	symbol_put(tb_ring_throttling);
	tbv_ring_throttling = NULL;
}

static bool apple_tx_raw_mode;
module_param(apple_tx_raw_mode, bool, 0644);
MODULE_PARM_DESC(apple_tx_raw_mode,
		 "Use RAW descriptors for Apple-compatible TX rings; default keeps FRAME descriptors");

static bool apple_tx_e2e;
module_param(apple_tx_e2e, bool, 0644);
MODULE_PARM_DESC(apple_tx_e2e,
		 "Enable E2E flow control on Apple-compatible TX rings");

static bool apple_rx_raw_mode;
module_param(apple_rx_raw_mode, bool, 0644);
MODULE_PARM_DESC(apple_rx_raw_mode,
		 "Use RAW descriptors for Apple-compatible RX rings; default keeps FRAME reassembly");

static bool destroy_disable_paths = true;
module_param(destroy_disable_paths, bool, 0644);
MODULE_PARM_DESC(destroy_disable_paths,
		 "Call tb_xdomain_disable_paths() during path teardown; set to 0 for recovery-oriented native testing");

/*
 * Map each zero-copy TX frame to the page boundary rather than to the exact
 * transmitted length, so an NHI read that bursts past frame->size cannot fault
 * the unmapped tail of the page (the copied path always maps a full 4096
 * buffer). Default on -- the fix for the 0.2.26 zcopy receiver-CRC storm.
 * Settable to 0 to A/B the exact-length mapping on hardware.
 */
static bool zcopy_map_full_page = true;
module_param(zcopy_map_full_page, bool, 0644);
MODULE_PARM_DESC(zcopy_map_full_page,
		 "Map zero-copy TX frames to the page boundary so an NHI over-read cannot fault past the mapping; 0 maps exactly the transmitted length");

/*
 * The NHI hardware CRC errors observed under the 32-QP bidirectional workload
 * overwhelmingly affect short, header-bearing native frames; full 4096-byte
 * descriptors complete cleanly. Send those self-delimiting frames from a
 * zero-filled full buffer while retaining the real payload length in their
 * native header. Raw stream frames and the Apple backend remain exact-sized.
 * Settable for immediate hardware A/B and rollback without rebuilding.
 */
static bool native_pad_short_frames;
module_param(native_pad_short_frames, bool, 0644);
MODULE_PARM_DESC(native_pad_short_frames,
		 "Pad ordinary short native frames to 4096 bytes on the wire; excludes raw streams and Apple framing");

static uint tx_stall_warn_ms = 5000;
module_param(tx_stall_warn_ms, uint, 0644);
MODULE_PARM_DESC(tx_stall_warn_ms,
		 "Warn when a native/apple TX ring has inflight frames but no completions for this many ms; 0 disables");

/*
 * Ceiling on how long a data packet may wait on tx_data_queue before it is
 * failed to its owner. Every data packet is gated on the software credit
 * window, which only advances on peer PATH_CREDIT frames; a peer that stops
 * returning credits leaves the queue with no other exit, and the packet's done
 * callback -- the only thing that drains the owning WR's tx_pending -- never
 * runs. Must exceed the QP retransmit budget so a healthy retransmit is never
 * cut short. 0 disables the ceiling (wait for credits forever).
 */
static uint tx_queue_timeout_ms = 30000;
module_param(tx_queue_timeout_ms, uint, 0644);
MODULE_PARM_DESC(tx_queue_timeout_ms,
		 "Fail a data packet that has waited this many ms on the path TX queue (credit stall); 0 waits forever");

/* Watchdog cadence for the queue ceiling; the ceiling itself is the deadline. */
#define TBV_TX_QUEUE_WATCHDOG_TICK_MS 1000U

/*
 * Interval at which a path re-advertises the ABSOLUTE count of data credits it
 * has returned (TBV_NATIVE_DATA_OP_PATH_CREDIT_SYNC). PATH_CREDIT carries a
 * delta on an unacknowledged single-shot control frame, so one frame the NHI
 * drops shortens the peer's window for the life of the path; a cumulative
 * count lets the peer recompute the shortfall from any single later frame, so
 * the window heals within one interval instead of never. Emitted only to peers
 * that advertised TBV_NATIVE_WIRE_CAP_CREDIT_SYNC. 0 disables the resync and
 * leaves the delta-only behaviour.
 */
static uint credit_resync_ms = 1000;
module_param(credit_resync_ms, uint, 0644);
MODULE_PARM_DESC(credit_resync_ms,
		 "Interval in ms at which a native path re-advertises its absolute returned-credit count; 0 disables");

/*
 * Skip a rail for new native QP binding when its TX ring has frames in
 * flight but has made no completion progress for this many ms. On the
 * single-cable TB3/TB4 daisy chain one of the two advertised native lanes
 * can come up "tunnel_enabled" yet never egress (ring posts, zero
 * completions); load-balancing fresh QPs onto it strands them. 0 disables
 * the health gate (pure round-robin).
 */
static uint tx_stall_skip_ms = 1500;
module_param(tx_stall_skip_ms, uint, 0644);
MODULE_PARM_DESC(tx_stall_skip_ms,
		 "Exclude a rail from new native QP binding when its TX ring has inflight frames but no completion progress for this many ms; 0 disables");

bool tbv_path_tx_stalled(const struct tbv_path *path)
{
	unsigned long progress;

	if (!tx_stall_skip_ms)
		return false;
	/* A ring that took frames and never completed them stays excluded
	 * until it completes something, even with nothing left inflight. */
	if (READ_ONCE(path->tx_ring_stalled))
		return true;
	if (atomic_read(&path->tx_inflight) <= 0)
		return false;
	progress = READ_ONCE(path->tx_last_progress_jiffies);
	return time_after(jiffies, progress + msecs_to_jiffies(tx_stall_skip_ms));
}

static bool tbv_path_mark_tx_recovery_attempted(struct tbv_path *path)
{
	unsigned long flags;
	bool first = false;

	if (!path->rail || !path->rail->peer ||
	    path->rail->peer->backend != TBV_BACKEND_NATIVE)
		return false;

	spin_lock_irqsave(&path->tx_lock, flags);
	if (!path->tx_recovery_attempted) {
		path->tx_recovery_attempted = true;
		first = true;
	}
	spin_unlock_irqrestore(&path->tx_lock, flags);
	return first;
}

struct tbv_data_frame {
	struct ring_frame frame;
	struct tbv_path *path;
	struct list_head free_node;
	void *buf;
	dma_addr_t dma;
	struct tbv_tx_packet *packet;
	tbv_path_tx_done_fn done;
	void *done_ctx;
	bool tx;
	/* When this frame was handed to the ring, for the stall ceiling. */
	unsigned long posted_jiffies;
	/*
	 * Set when tbv_path_expire_tx_ring_frames() gave up on this frame and
	 * already accounted its inflight slot. The ring still owns the buffer
	 * and may complete it later; that late completion must not decrement
	 * tx_inflight a second time.
	 */
	bool released;
};

struct tbv_tx_packet {
	struct list_head node;
	struct tbv_path *path;
	u8 *buf;
	u32 len;
	struct ring_frame frame;
	dma_addr_t dma;
	/*
	 * Bytes actually dma_map'd for a zcopy frame. May exceed len (the
	 * transmitted frame->size) when the frame is mapped to the page
	 * boundary so an NHI over-read cannot fault past the mapping; unmap
	 * must use this, not len.
	 */
	u32 dma_len;
	tbv_path_tx_done_fn done;
	void *done_ctx;
	void *owner_ctx;
	u8 sof;
	u8 eof;
	u32 start_credit_group_frames;
	unsigned long queued_jiffies;
	bool control;
	bool pooled;
	bool queued;
	bool inflight;
	bool zcopy;
	bool unmap_dma;
	bool inline_buf;
	bool raw_stream_start;
	bool raw_stream_end;
	bool raw_stream_counted;
	u8 control_buf[TBV_CONTROL_FRAME_SIZE];
};

static u32 tbv_frame_len(const struct ring_frame *frame)
{
	return frame->size ? frame->size : (u32)TBV_DATA_FRAME_SIZE;
}

static struct tbv_state *tbv_path_state(struct tbv_path *path)
{
	return path->rail && path->rail->peer ? path->rail->peer->state : NULL;
}

static u32 tbv_path_control_packet_count(const struct tbv_path *path)
{
	u32 count = path->cfg.tx_ring_size * TBV_CONTROL_QUEUE_MULTIPLIER;

	return clamp_t(u32, count, 64, 4096);
}

static u32 tbv_path_data_packet_count(const struct tbv_path *path)
{
	u32 count = path->cfg.tx_ring_size * TBV_DATA_QUEUE_MULTIPLIER;

	return min_t(u32, count, TBV_DATA_PACKET_POOL_LIMIT);
}

static u32 tbv_path_tx_control_frame_reserve(const struct tbv_path *path)
{
	u32 reserve;

	if (path->tx_frame_count <= 1)
		return 0;

	reserve = path->tx_frame_count / 4;
	return clamp_t(u32, reserve, 1, TBV_DATA_CREDIT_CONTROL_RESERVE);
}

static int tbv_path_configure_ring_throttling(struct tbv_path *path)
{
	u32 interval = READ_ONCE(nhi_interrupt_throttle_ns);
	int ret;

	if (!tbv_ring_throttling) {
		if (interval)
			pr_warn_once("nhi_interrupt_throttle_ns requires a kernel exporting tb_ring_throttling(); ignoring interval %u ns\n",
				     interval);
		return 0;
	}

	ret = tbv_ring_throttling(path->tx_ring, interval);
	if (ret) {
		pr_warn("TX ring throttling interval %u ns failed ret=%d\n",
			interval, ret);
		return ret;
	}

	ret = tbv_ring_throttling(path->rx_ring, interval);
	if (ret) {
		pr_warn("RX ring throttling interval %u ns failed ret=%d\n",
			interval, ret);
		return ret;
	}

	return 0;
}

static void tbv_path_tx_packet_release(struct tbv_tx_packet *packet, int status)
{
	struct tbv_path *path = packet->path;
	unsigned long flags;

	if (packet->zcopy && packet->unmap_dma) {
		struct device *dma_dev = tb_ring_dma_device(path->tx_ring);

		if (tbv_dma_device_ready(dma_dev))
			dma_unmap_page(dma_dev, packet->dma,
				       packet->dma_len ? packet->dma_len :
							 packet->len,
				       DMA_TO_DEVICE);
		else
			pr_warn_ratelimited("TX ring DMA device is not ready for zcopy unmapping\n");
	}

	if (packet->done)
		packet->done(packet->done_ctx, status);

	packet->done = NULL;
	packet->done_ctx = NULL;
	packet->owner_ctx = NULL;
	packet->len = 0;
	packet->start_credit_group_frames = 0;
	packet->queued_jiffies = 0;
	packet->queued = false;
	packet->inflight = false;

	if (packet->zcopy) {
		kfree(packet);
		return;
	}

	if (!packet->control && packet->pooled) {
		spin_lock_irqsave(&path->tx_lock, flags);
		list_add_tail(&packet->node, &path->tx_data_free);
		spin_unlock_irqrestore(&path->tx_lock, flags);
		return;
	}

	if (!packet->control) {
		if (!packet->inline_buf)
			kfree(packet->buf);
		kfree(packet);
		return;
	}
	if (!packet->pooled) {
		kfree(packet);
		return;
	}

	packet->buf = packet->control_buf;
	spin_lock_irqsave(&path->tx_lock, flags);
	list_add_tail(&packet->node, &path->tx_control_free);
	spin_unlock_irqrestore(&path->tx_lock, flags);
}

static void tbv_path_schedule_tx(struct tbv_path *path);
static void tbv_path_tx_poll_work(struct work_struct *work);
static void tbv_path_rx_supp_poll_work(struct work_struct *work);
static void tbv_path_credit_sync_work(struct work_struct *work);

static bool tbv_path_progress_poll_enabled(const struct tbv_path *path)
{
	if (!path->rail || !path->rail->peer)
		return false;

	return path->rail->peer->backend == TBV_BACKEND_NATIVE ||
	       path->rail->peer->backend == TBV_BACKEND_APPLE;
}

static void tbv_path_queue_delayed_work(struct tbv_path *path,
					struct delayed_work *work,
					unsigned long delay)
{
	struct tbv_state *state = tbv_path_state(path);

	if (state && state->workqueue)
		queue_delayed_work(state->workqueue, work, delay);
	else
		schedule_delayed_work(work, delay);
}

static void tbv_path_queue_tx_poll(struct tbv_path *path, unsigned long delay)
{
	if (!path->tx_poll_enabled || !path->tx_ring)
		return;
	/* 0 = poller disabled: interrupt-driven ring_work still completes TX. */
	if (!READ_ONCE(tx_poll_delay_ms))
		return;

	tbv_path_queue_delayed_work(path, &path->tx_poll_work, delay);
}

/*
 * Arm the TX delayed work for the queue ceiling. Independent of
 * tx_poll_delay_ms: that parameter tunes the supplemental COMPLETION poller
 * (0 = rely on interrupts) and disabling it must not disable the ceiling,
 * which is the only exit a credit-stalled packet has. queue_delayed_work is a
 * no-op while the work is already pending, so a tick already armed for
 * completion polling serves both.
 */
static void tbv_path_arm_queue_watchdog(struct tbv_path *path)
{
	u32 ms = READ_ONCE(tx_queue_timeout_ms);

	if (!ms || !path->tx_poll_enabled || !path->tx_ring)
		return;

	tbv_path_queue_delayed_work(path, &path->tx_poll_work,
				    msecs_to_jiffies(min(ms,
					TBV_TX_QUEUE_WATCHDOG_TICK_MS)));
}

static void tbv_path_queue_rx_supp_poll(struct tbv_path *path,
					unsigned long delay)
{
	if (!path->rx_supp_poll_enabled || !path->rx_ring)
		return;

	WRITE_ONCE(path->rx_supp_poll_until,
		   jiffies + msecs_to_jiffies(TBV_RX_SUPP_POLL_WINDOW_MS));
	tbv_path_queue_delayed_work(path, &path->rx_supp_poll_work, delay);
}

static void tbv_path_atomic64_max_ms(atomic64_t *counter, u64 value)
{
	s64 old;

	if (value > S64_MAX)
		value = S64_MAX;

	for (;;) {
		old = atomic64_read(counter);
		if (old >= (s64)value)
			return;
		if (atomic64_cmpxchg(counter, old, (s64)value) == old)
			return;
	}
}

static u32 tbv_path_data_credit_window(u32 rx_ring_size)
{
	u32 credits;

	if (!rx_ring_size)
		return 0;

	if (rx_ring_size <= TBV_DATA_CREDIT_CONTROL_RESERVE)
		credits = rx_ring_size / 2;
	else
		credits = rx_ring_size - TBV_DATA_CREDIT_CONTROL_RESERVE;

	if (credits > TBV_NATIVE_DATA_CREDIT_BATCH)
		credits -= credits % TBV_NATIVE_DATA_CREDIT_BATCH;
	if (!credits)
		credits = 1;

	return credits;
}

static bool tbv_path_uses_software_credits(const struct tbv_path *path)
{
	const struct tbv_peer *peer;

	if (!path->cfg.e2e || !READ_ONCE(path->remote_e2e))
		return true;
	if (!path->rail || !path->rail->peer)
		return true;

	peer = path->rail->peer;
	if (peer->backend != TBV_BACKEND_NATIVE)
		return true;

	return !(READ_ONCE(peer->remote_caps) &
		 TBV_NATIVE_WIRE_CAP_E2E_NO_SW_CREDIT);
}

void tbv_path_set_remote_rx_capacity(struct tbv_path *path, u32 rx_ring_size)
{
	unsigned long flags;
	u32 credits;

	if (!path)
		return;

	credits = tbv_path_uses_software_credits(path) ?
		  tbv_path_data_credit_window(rx_ring_size) : 0;
	spin_lock_irqsave(&path->tx_lock, flags);
	path->tx_remote_data_credit_max = credits;
	path->tx_remote_data_credits = credits;
	path->rx_data_credit_pending = 0;
	/*
	 * Both cumulative counters restart with the window. The peer's counter
	 * restarts independently, so the next resync only adopts its baseline
	 * (the window is already full here and owes nothing).
	 */
	path->rx_data_credit_returned_total = 0;
	path->tx_remote_credit_returned_seen = 0;
	path->tx_credit_sync_primed = false;
	spin_unlock_irqrestore(&path->tx_lock, flags);

	tbv_path_schedule_tx(path);
}

void tbv_path_add_remote_rx_credits(struct tbv_path *path, u32 credits)
{
	struct tbv_state *state;
	unsigned long flags;
	u32 accepted = 0;
	u32 old;
	u32 new;

	if (!path || !credits)
		return;

	state = tbv_path_state(path);
	spin_lock_irqsave(&path->tx_lock, flags);
	if (path->tx_remote_data_credit_max) {
		old = path->tx_remote_data_credits;
		if (old >= path->tx_remote_data_credit_max)
			new = path->tx_remote_data_credit_max;
		else if (credits > path->tx_remote_data_credit_max - old)
			new = path->tx_remote_data_credit_max;
		else
			new = old + credits;
		path->tx_remote_data_credits = new;
		accepted = new - old;
		/*
		 * Track the peer's count, not what was accepted: the clamp
		 * discards credits the window cannot hold, and a later resync
		 * must not re-grant them.
		 */
		path->tx_remote_credit_returned_seen += credits;
	}
	spin_unlock_irqrestore(&path->tx_lock, flags);

	if (!accepted)
		return;

	if (state)
		atomic64_add(accepted, &state->data_tx_credit_received);
	atomic64_add(accepted, &path->data_tx_credit_received);
	tbv_path_schedule_tx(path);
}

/*
 * Reclaim credits for a message about to be retransmitted. A data frame charges
 * one credit when it leaves (tbv_path_schedule_tx); the peer returns it only
 * when that frame is RECEIVED (tbv_path_rx_complete). A frame lost in transit is
 * therefore charged but never returned, and the full-message retransmit charges
 * a fresh credit for it -- a permanent one-credit-per-lost-frame leak that
 * drains the window over a long ping-pong (the 16K ib_send_lat hang at the
 * default 768 window). Refunding the failed attempt's frames before the
 * retransmit re-charges them heals the leak; the max cap absorbs the
 * over-refund for frames that DID arrive (the peer already returned those), so
 * the sender can never exceed the peer's ring depth. See credit_pingpong_test.c.
 */
void tbv_path_refund_remote_data_credits(struct tbv_path *path, u32 frames)
{
	unsigned long flags;

	if (!path || !frames)
		return;

	spin_lock_irqsave(&path->tx_lock, flags);
	if (path->tx_remote_data_credit_max)
		path->tx_remote_data_credits = tbv_native_data_refund_credits(
			path->tx_remote_data_credits,
			path->tx_remote_data_credit_max, frames);
	spin_unlock_irqrestore(&path->tx_lock, flags);

	tbv_path_schedule_tx(path);
}

/*
 * Apply a peer's absolute returned-credit count. A backward serial is stale,
 * not a four-billion-credit wrap. Advance the observed total and restore the
 * balance atomically: dropping the lock between computing the delta and
 * applying it lets a concurrent PATH_CREDIT update the same ledger twice.
 * The first resync after a capacity change only adopts the baseline, since the
 * two counters restart independently across a re-HELLO.
 */
void tbv_path_sync_remote_rx_credits(struct tbv_path *path, u32 total)
{
	struct tbv_state *state;
	unsigned long flags;
	u32 delta = 0;
	u32 accepted = 0;

	if (!path)
		return;

	state = tbv_path_state(path);
	spin_lock_irqsave(&path->tx_lock, flags);
	if (!path->tx_remote_data_credit_max) {
		spin_unlock_irqrestore(&path->tx_lock, flags);
		return;
	}
	if (!path->tx_credit_sync_primed) {
		path->tx_credit_sync_primed = true;
		path->tx_remote_credit_returned_seen = total;
	} else if (tbv_native_data_resync_forward(
			   path->tx_remote_credit_returned_seen, total,
			   &delta)) {
		u32 room = path->tx_remote_data_credits >=
				   path->tx_remote_data_credit_max ?
				   0 : path->tx_remote_data_credit_max -
					       path->tx_remote_data_credits;

		path->tx_remote_credit_returned_seen = total;
		accepted = min(delta, room);
		path->tx_remote_data_credits += accepted;
	}
	spin_unlock_irqrestore(&path->tx_lock, flags);

	if (!accepted)
		return;

	if (state)
		atomic64_add(accepted,
			     &state->data_tx_credit_resync_recovered);
	atomic64_add(accepted, &path->data_tx_credit_resync_recovered);
	if (state)
		atomic64_add(accepted, &state->data_tx_credit_received);
	atomic64_add(accepted, &path->data_tx_credit_received);
	tbv_path_schedule_tx(path);
}

static bool tbv_path_credit_sync_supported(const struct tbv_path *path)
{
	if (!tbv_path_uses_software_credits(path))
		return false;
	if (!path->rail || !path->rail->peer)
		return false;
	if (path->rail->peer->backend != TBV_BACKEND_NATIVE)
		return false;

	return !!(READ_ONCE(path->rail->peer->remote_caps) &
		  TBV_NATIVE_WIRE_CAP_CREDIT_SYNC);
}

static int tbv_path_send_rx_credit(struct tbv_path *path, u32 credits)
{
	struct tbv_native_data_header hdr = {};
	u8 frame[TBV_NATIVE_DATA_HDR_SIZE];
	int len;

	hdr.opcode = TBV_NATIVE_DATA_OP_PATH_CREDIT;
	hdr.imm_data = credits;

	len = tbv_native_data_build_header(frame, sizeof(frame), &hdr);
	if (len < 0)
		return len;

	return tbv_path_send(path, frame, len, TBV_PATH_SEND_CONTROL, NULL, NULL);
}

static int tbv_path_send_rx_credit_sync(struct tbv_path *path, u32 total)
{
	struct tbv_native_data_header hdr = {};
	u8 frame[TBV_NATIVE_DATA_HDR_SIZE];
	int len;

	hdr.opcode = TBV_NATIVE_DATA_OP_PATH_CREDIT_SYNC;
	hdr.frag_offset = total;

	len = tbv_native_data_build_header(frame, sizeof(frame), &hdr);
	if (len < 0)
		return len;

	return tbv_path_send(path, frame, len, TBV_PATH_SEND_CONTROL, NULL, NULL);
}

static void tbv_path_arm_credit_sync(struct tbv_path *path)
{
	u32 ms = READ_ONCE(credit_resync_ms);

	if (!ms || path->state != TBV_PATH_TUNNEL_ENABLED)
		return;
	if (!tbv_path_credit_sync_supported(path))
		return;

	tbv_path_queue_delayed_work(path, &path->credit_sync_work,
				    msecs_to_jiffies(ms));
}

/*
 * Periodic absolute re-advertisement. It runs on its own timer rather than on
 * the TX poller because the side that OWES credits may have no TX traffic of
 * its own, and the peer's window is exactly what stalls when it hears nothing.
 * The count is monotone, so a resync that is itself dropped costs only one
 * interval.
 */
static void tbv_path_credit_sync_work(struct work_struct *work)
{
	struct tbv_path *path = container_of(to_delayed_work(work),
					     struct tbv_path, credit_sync_work);
	struct tbv_state *state = tbv_path_state(path);
	unsigned long flags;
	u32 total;
	int ret;

	if (path->state != TBV_PATH_TUNNEL_ENABLED)
		return;

	spin_lock_irqsave(&path->tx_lock, flags);
	total = path->rx_data_credit_returned_total;
	spin_unlock_irqrestore(&path->tx_lock, flags);

	if (total && tbv_path_credit_sync_supported(path)) {
		ret = tbv_path_send_rx_credit_sync(path, total);
		if (!ret) {
			if (state)
				atomic64_inc(&state->data_rx_credit_resynced);
			atomic64_inc(&path->data_rx_credit_resynced);
		}
	}

	tbv_path_arm_credit_sync(path);
}

static void tbv_path_return_rx_data_credit(struct tbv_path *path, u32 credits)
{
	struct tbv_state *state;
	unsigned long flags;
	u32 threshold;
	u32 pending;
	u32 send = 0;
	int ret;

	if (!path || !credits)
		return;
	if (!tbv_path_uses_software_credits(path))
		return;

	state = tbv_path_state(path);
	threshold = tbv_native_data_credit_return_threshold(
		tbv_path_data_credit_window(path->cfg.rx_ring_size));
	spin_lock_irqsave(&path->tx_lock, flags);
	/*
	 * Counts eligibility, not transmission: the peer must be told about a
	 * credit whose PATH_CREDIT frame is lost, and that is the whole point
	 * of the cumulative count. Wraps freely (the peer works in deltas).
	 */
	path->rx_data_credit_returned_total += credits;
	pending = path->rx_data_credit_pending;
	if (credits > U32_MAX - pending)
		pending = U32_MAX;
	else
		pending += credits;
	if (pending >= threshold) {
		send = pending;
		pending = 0;
	}
	path->rx_data_credit_pending = pending;
	spin_unlock_irqrestore(&path->tx_lock, flags);

	if (!send)
		return;

	ret = tbv_path_send_rx_credit(path, send);
	if (ret) {
		spin_lock_irqsave(&path->tx_lock, flags);
		pending = path->rx_data_credit_pending;
		if (send > U32_MAX - pending)
			path->rx_data_credit_pending = U32_MAX;
		else
			path->rx_data_credit_pending = pending + send;
		spin_unlock_irqrestore(&path->tx_lock, flags);
		if (state)
			atomic64_inc(&state->data_rx_credit_send_error);
		atomic64_inc(&path->data_rx_credit_send_error);
		return;
	}

	if (state)
		atomic64_add(send, &state->data_rx_credit_sent);
	atomic64_add(send, &path->data_rx_credit_sent);
}

static bool tbv_native_data_consumes_rx_credit(u8 opcode)
{
	switch (opcode) {
	case TBV_NATIVE_DATA_OP_SEND:
	case TBV_NATIVE_DATA_OP_SEND_IMM:
	case TBV_NATIVE_DATA_OP_RDMA_WRITE:
	case TBV_NATIVE_DATA_OP_RDMA_WRITE_IMM:
	case TBV_NATIVE_DATA_OP_RDMA_READ_REQ:
	case TBV_NATIVE_DATA_OP_RDMA_READ_RESP:
	case TBV_NATIVE_DATA_OP_MAD:
		return true;
	default:
		return false;
	}
}

static bool
tbv_native_data_valid_path_credit(const struct tbv_native_data_header *hdr)
{
	return hdr->opcode == TBV_NATIVE_DATA_OP_PATH_CREDIT &&
	       hdr->imm_data &&
	       !hdr->flags &&
	       !hdr->dest_qp &&
	       !hdr->src_qp &&
	       !hdr->psn &&
	       !hdr->length &&
	       !hdr->remote_addr &&
	       !hdr->rkey;
}

static void tbv_path_count_raw_stream_locked(struct tbv_path *path,
					     struct tbv_tx_packet *packet)
{
	if (packet->raw_stream_start) {
		/*
		 * A chained window of the SAME owner (per-fragment split
		 * streams) must not reset the inflight count: the previous
		 * window's completions are still pending against it.
		 */
		if (!path->tx_raw_stream_active ||
		    path->tx_raw_stream_owner != packet->owner_ctx) {
			path->tx_raw_stream_active = true;
			path->tx_raw_stream_owner = packet->owner_ctx;
			path->tx_raw_stream_inflight = 0;
		}
		path->tx_raw_stream_end_seen = false;
		path->tx_raw_stream_window_open = true;
	}
	if (path->tx_raw_stream_active &&
	    packet->owner_ctx == path->tx_raw_stream_owner) {
		path->tx_raw_stream_inflight++;
		packet->raw_stream_counted = true;
		/*
		 * The end packet's DEQUEUE closes the unframed window: from
		 * here the ring order is safe for unrelated frames again.
		 */
		if (packet->raw_stream_end)
			path->tx_raw_stream_window_open = false;
	}
}

static void tbv_path_finish_raw_stream_if_needed(struct tbv_path *path,
						 struct tbv_tx_packet *packet)
{
	unsigned long flags;

	if (!packet || !packet->raw_stream_counted)
		return;

	spin_lock_irqsave(&path->tx_lock, flags);
	if (path->tx_raw_stream_owner == packet->owner_ctx) {
		if (packet->raw_stream_end)
			path->tx_raw_stream_end_seen = true;
		if (path->tx_raw_stream_inflight)
			path->tx_raw_stream_inflight--;
		if (path->tx_raw_stream_end_seen &&
		    !path->tx_raw_stream_inflight) {
			path->tx_raw_stream_active = false;
			path->tx_raw_stream_owner = NULL;
			path->tx_raw_stream_end_seen = false;
		}
	}
	packet->raw_stream_counted = false;
	if (!path->tx_raw_stream_active) {
		path->tx_raw_stream_active = false;
		path->tx_raw_stream_owner = NULL;
		path->tx_raw_stream_end_seen = false;
		path->tx_raw_stream_window_open = false;
		path->tx_raw_stream_inflight = 0;
	}
	spin_unlock_irqrestore(&path->tx_lock, flags);
}

bool tbv_path_apple_tx_raw_mode(void)
{
	return READ_ONCE(apple_tx_raw_mode);
}

bool tbv_path_apple_rx_raw_mode(void)
{
	return READ_ONCE(apple_rx_raw_mode);
}

void tbv_path_default_config(enum tbv_backend_type backend,
			     struct tbv_path_config *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	cfg->tx_hop = -1;
	cfg->rx_hop = -1;
	cfg->transmit_path = -1;
	cfg->receive_path = -1;

	switch (backend) {
	case TBV_BACKEND_APPLE:
		cfg->tx_ring_size = TBV_APPLE_RING_SIZE;
		cfg->rx_ring_size = TBV_APPLE_RING_SIZE;
		cfg->tx_flags = 0;
		if (!tbv_path_apple_tx_raw_mode())
			cfg->tx_flags |= RING_FLAG_FRAME;
		if (READ_ONCE(apple_tx_e2e))
			cfg->tx_flags |= RING_FLAG_E2E;
		cfg->rx_flags = RING_FLAG_E2E;
		if (!tbv_path_apple_rx_raw_mode())
			cfg->rx_flags |= RING_FLAG_FRAME;
		cfg->tx_hop = 2;
		cfg->rx_hop = 2;
		cfg->transmit_path = 9;
		cfg->receive_path = 9;
		if (tbv_path_apple_rx_raw_mode()) {
			cfg->sof_mask = 0xffff;
			cfg->eof_mask = 0xffff;
		} else {
			cfg->sof_mask = BIT(1);
			cfg->eof_mask = BIT(2) | BIT(3);
		}
		cfg->e2e = true;
		break;

	case TBV_BACKEND_NATIVE:
	default:
		cfg->tx_ring_size = native_ring_size;
		cfg->rx_ring_size = native_ring_size;
		cfg->tx_flags = RING_FLAG_FRAME;
		cfg->rx_flags = RING_FLAG_FRAME;
		cfg->sof_mask = BIT(1);
		cfg->eof_mask = BIT(2) | BIT(3);
		break;
	}
}

void tbv_path_init(struct tbv_path *path,
		   const struct tbv_path_config *cfg, struct tbv_rail *rail)
{
	memset(path, 0, sizeof(*path));
	path->state = TBV_PATH_NEW;
	path->cfg = *cfg;
	path->rail = rail;
	spin_lock_init(&path->tx_lock);
	INIT_LIST_HEAD(&path->tx_free);
	INIT_LIST_HEAD(&path->tx_control_free);
	INIT_LIST_HEAD(&path->tx_data_free);
	INIT_LIST_HEAD(&path->tx_control_queue);
	INIT_LIST_HEAD(&path->tx_data_queue);
	INIT_LIST_HEAD(&path->tx_zcopy_inflight);
	INIT_LIST_HEAD(&path->tx_frame_inflight);
	INIT_DELAYED_WORK(&path->tx_poll_work, tbv_path_tx_poll_work);
	INIT_DELAYED_WORK(&path->rx_supp_poll_work,
			  tbv_path_rx_supp_poll_work);
	INIT_DELAYED_WORK(&path->credit_sync_work, tbv_path_credit_sync_work);
	init_waitqueue_head(&path->tx_idle_wait);
	atomic_set(&path->tx_inflight, 0);
	path->local_transmit_path = -1;
	path->local_tx_hop = -1;
	path->local_rx_hop = -1;
	path->remote_transmit_path = -1;
}

void tbv_path_reset(struct tbv_path *path)
{
	path->tx_ring = NULL;
	path->rx_ring = NULL;
	memset(path, 0, sizeof(*path));
	path->state = TBV_PATH_STOPPED;
	spin_lock_init(&path->tx_lock);
	INIT_LIST_HEAD(&path->tx_free);
	INIT_LIST_HEAD(&path->tx_control_free);
	INIT_LIST_HEAD(&path->tx_data_free);
	INIT_LIST_HEAD(&path->tx_control_queue);
	INIT_LIST_HEAD(&path->tx_data_queue);
	INIT_LIST_HEAD(&path->tx_zcopy_inflight);
	INIT_LIST_HEAD(&path->tx_frame_inflight);
	INIT_DELAYED_WORK(&path->tx_poll_work, tbv_path_tx_poll_work);
	INIT_DELAYED_WORK(&path->rx_supp_poll_work,
			  tbv_path_rx_supp_poll_work);
	INIT_DELAYED_WORK(&path->credit_sync_work, tbv_path_credit_sync_work);
	init_waitqueue_head(&path->tx_idle_wait);
	atomic_set(&path->tx_inflight, 0);
	path->local_transmit_path = -1;
	path->local_tx_hop = -1;
	path->local_rx_hop = -1;
	path->remote_transmit_path = -1;
}

static void tbv_path_tx_complete(struct tb_ring *ring, struct ring_frame *frame,
				 bool canceled)
{
	struct tbv_data_frame *f = container_of(frame, struct tbv_data_frame,
						frame);
	struct tbv_path *path = f->path;
	struct tbv_tx_packet *packet;
	struct tbv_state *state = tbv_path_state(path);
	unsigned long flags;
	bool released;

	dma_sync_single_for_cpu(tb_ring_dma_device(ring), f->dma,
				TBV_DATA_FRAME_SIZE, DMA_TO_DEVICE);
	/*
	 * Ring progress, stamped where the ring hands the frame back and not
	 * where the driver happens to reap it. The TX ring is interrupt-driven
	 * (ring_work) and the supplemental poll only sees the small tail that
	 * quiesces between interrupts, so stamping this in the poll made a
	 * perfectly healthy ring look stalled to tbv_path_tx_stalled() and to
	 * the poll's own warning.
	 */
	WRITE_ONCE(path->tx_last_progress_jiffies, jiffies);
	spin_lock_irqsave(&path->tx_lock, flags);
	released = f->released;
	f->released = false;
	packet = f->packet;
	f->packet = NULL;
	f->done = NULL;
	f->done_ctx = NULL;
	f->frame.callback = NULL;
	f->frame.size = 0;
	f->frame.flags = 0;
	f->frame.sof = 0;
	f->frame.eof = 0;
	f->posted_jiffies = 0;
	path->tx_ring_stalled = false;
	path->tx_recovery_attempted = false;
	list_del_init(&f->free_node);
	list_add_tail(&f->free_node, &path->tx_free);
	spin_unlock_irqrestore(&path->tx_lock, flags);

		if (state) {
			if (canceled)
				atomic64_inc(&state->data_tx_canceled);
			else
				atomic64_inc(&state->data_tx_completed);
		}
		if (packet && !canceled) {
			if (packet->control) {
				u64 age_ms = packet->queued_jiffies ?
					jiffies_to_msecs(jiffies -
							 packet->queued_jiffies) : 0;

				atomic64_inc(&path->control_tx_completed);
				tbv_path_atomic64_max_ms(
					&path->control_tx_queue_max_ms, age_ms);
			} else {
				atomic64_inc(&path->data_tx_completed);
			}
		}
	if (packet) {
		tbv_path_finish_raw_stream_if_needed(path, packet);
		tbv_path_tx_packet_release(packet,
					   canceled ? -ECANCELED : 0);
	}

	/*
	 * A frame the ceiling already released has had its inflight slot given
	 * back. Decrementing again drives tx_inflight negative, and
	 * tbv_path_schedule_tx() reads that counter into a u32 -- one stray
	 * decrement leaves the staged-frame gate computing zero available
	 * frames forever, so the path never posts a copied data frame again.
	 */
	if (!released)
		atomic_dec(&path->tx_inflight);
	tbv_path_schedule_tx(path);
}

/*
 * Fail data packets that have waited on tx_data_queue past tx_queue_timeout_ms.
 *
 * The queue is FIFO by enqueue time (every enqueue is list_add_tail; the
 * -ENOMEM requeue puts back the packet it just took off the head), so the head
 * is the oldest and the scan stops at the first entry still inside the
 * ceiling. Releasing with -ETIMEDOUT runs the packet's done callback, which is
 * what drains the owning WR's tx_pending and lets the QP surface the failure;
 * a packet the credit gate never admits has no other way out. Reports the
 * remaining queue depth so the caller can decide to re-arm.
 */
/*
 * Release copied frames the ring accepted but never completed.
 *
 * tbv_path_expire_tx_data_queue() only sees packets still waiting for credits
 * on tx_data_queue. A packet handed to the ring has already left that queue,
 * and a copied packet is not on tx_zcopy_inflight either, so a ring that stops
 * completing strands it with nothing to run its done callback -- the owning WR
 * never drains tx_pending and every later WR on the rail queues behind it.
 *
 * Measure from posted_jiffies, which the post stamps and nothing resets. Do
 * not key this off tx_last_progress_jiffies: that is a per-path stamp of the
 * last completion of ANY frame, so a path completing other frames around one
 * stuck descriptor would never look stalled here.
 */
static u32 tbv_path_expire_tx_ring_frames(struct tbv_path *path)
{
	unsigned long timeout = msecs_to_jiffies(READ_ONCE(tx_queue_timeout_ms));
	struct tbv_data_frame *f;
	struct tbv_data_frame *tmp;
	struct tbv_tx_packet *packet;
	unsigned long flags;
	LIST_HEAD(expired);
	u32 count = 0;

	if (!timeout)
		return 0;

	spin_lock_irqsave(&path->tx_lock, flags);
	list_for_each_entry_safe(f, tmp, &path->tx_frame_inflight, free_node) {
		if (!f->posted_jiffies ||
		    !time_after(jiffies, f->posted_jiffies + timeout))
			continue;
		/*
		 * Complete the ownership transition while holding tx_lock.
		 *
		 * Do not move f->free_node to a temporary list and drop the
		 * lock: the ring completion callback is allowed to run then,
		 * remove that same node and return the frame to tx_free. The
		 * timeout worker would subsequently operate on a recycled frame
		 * and leave its stack-local list corrupted. The next expiry scan
		 * can then loop forever with IRQs disabled (the hard lockup seen
		 * on appmana-023).
		 *
		 * The ring still owns the frame, so leave its node detached and
		 * mark it released. A late completion sees released, returns the
		 * frame to tx_free, and does not spend the inflight slot twice.
		 * The packet is ours after f->packet is cleared; its node is
		 * otherwise detached while the copied frame is in the ring, so
		 * use that node for deferred completion outside the spinlock.
		 */
		list_del_init(&f->free_node);
		packet = f->packet;
		f->packet = NULL;
		f->done = NULL;
		f->done_ctx = NULL;
		f->posted_jiffies = 0;
		f->released = true;
		if (packet)
			list_add_tail(&packet->node, &expired);
		count++;
	}
	if (count)
		path->tx_ring_stalled = true;
	spin_unlock_irqrestore(&path->tx_lock, flags);

	/*
	 * Hand every expired frame's slot back before invoking owner callbacks.
	 * A concurrent late ring completion observes f->released and therefore
	 * skips its decrement; the poll caller schedules the newly available
	 * frames after this function returns.
	 */
	if (count)
		atomic_sub(count, &path->tx_inflight);

	while (!list_empty(&expired)) {
		packet = list_first_entry(&expired, struct tbv_tx_packet, node);
		list_del_init(&packet->node);
		tbv_path_tx_packet_release(packet, -ETIMEDOUT);
	}

	if (count) {
		pr_warn_ratelimited("tx ring stalled route=0x%llx rail=0x%x: released %u frame(s) the ring never completed\n",
				    path->rail && path->rail->peer ? path->rail->peer->xd->route : 0,
				    path->rail ? path->rail->rail_id : 0xffffffff,
				    count);
	}
	return count;
}

static u32 tbv_path_expire_tx_data_queue(struct tbv_path *path, u32 *queued_out)
{
	unsigned long timeout = msecs_to_jiffies(READ_ONCE(tx_queue_timeout_ms));
	struct tbv_tx_packet *packet;
	struct tbv_tx_packet *tmp;
	unsigned long flags;
	LIST_HEAD(expired);
	u32 count = 0;

	spin_lock_irqsave(&path->tx_lock, flags);
	if (timeout) {
		list_for_each_entry_safe(packet, tmp, &path->tx_data_queue,
					 node) {
			if (!packet->queued_jiffies ||
			    !time_after(jiffies,
					packet->queued_jiffies + timeout))
				break;
			list_del_init(&packet->node);
			packet->queued = false;
			if (path->tx_data_queued)
				path->tx_data_queued--;
			/*
			 * The rest of an unframed window can never be posted
			 * now, so the window must not stay open or every later
			 * packet on this path is blocked behind it.
			 */
			if (path->tx_raw_stream_active &&
			    path->tx_raw_stream_owner == packet->owner_ctx) {
				path->tx_raw_stream_active = false;
				path->tx_raw_stream_owner = NULL;
				path->tx_raw_stream_end_seen = false;
				path->tx_raw_stream_window_open = false;
				path->tx_raw_stream_inflight = 0;
			}
			list_add_tail(&packet->node, &expired);
			count++;
		}
	}
	if (queued_out)
		*queued_out = path->tx_data_queued;
	spin_unlock_irqrestore(&path->tx_lock, flags);

	if (count)
		pr_warn_ratelimited("tx queue timeout route=0x%llx rail=0x%x failed=%u queued=%u credits=%u/%u stalls=%lld\n",
				    path->rail && path->rail->peer ?
					    path->rail->peer->xd->route : 0,
				    path->rail ? path->rail->rail_id : 0xffffffff,
				    count, path->tx_data_queued,
				    path->tx_remote_data_credits,
				    path->tx_remote_data_credit_max,
				    atomic64_read(&path->data_tx_credit_stalls));

	while (!list_empty(&expired)) {
		packet = list_first_entry(&expired, struct tbv_tx_packet, node);
		list_del_init(&packet->node);
		tbv_path_tx_packet_release(packet, -ETIMEDOUT);
	}

	return count;
}

static void tbv_path_tx_poll_work(struct work_struct *work)
{
	struct tbv_path *path = container_of(to_delayed_work(work),
					     struct tbv_path, tx_poll_work);
	struct tb_ring *ring = READ_ONCE(path->tx_ring);
	struct ring_frame *frame;
	bool stalled;
	u64 completed = 0;
	u32 queued = 0;
	static DEFINE_RATELIMIT_STATE(stall_rs, HZ, 1);

	/*
	 * Before the ring poll and not gated on the ring: a credit-stalled
	 * queue holds packets that never reached the ring at all, and it must
	 * still drain when the path is being torn down.
	 */
	if (tbv_path_expire_tx_data_queue(path, &queued))
		tbv_path_schedule_tx(path);
	/*
	 * Also release frames the ring took and never completed; those are
	 * invisible to the queue expiry above because they already left it.
	 */
	if (tbv_path_expire_tx_ring_frames(path))
		tbv_path_schedule_tx(path);

	if (!ring) {
		if (queued)
			tbv_path_arm_queue_watchdog(path);
		return;
	}

	atomic64_inc(&path->tx_poll_calls);
	while ((frame = tb_ring_poll(ring))) {
		if (frame->callback)
			frame->callback(ring, frame, false);
		completed++;
	}
	/*
	 * Do not stamp progress here. The completion callbacks do it, whichever
	 * context reaps them, so a poll that finds nothing because the
	 * interrupt path already drained the ring no longer looks like a stall.
	 */
	if (completed) {
		atomic64_add(completed, &path->tx_poll_completed);
	} else {
		stalled = tx_stall_warn_ms &&
			  atomic_read(&path->tx_inflight) > 0 &&
			  time_after(jiffies,
				     READ_ONCE(path->tx_last_progress_jiffies) +
				     msecs_to_jiffies(tx_stall_warn_ms));
		if (stalled) {
			/*
			 * The warning used to be the end of the path-level
			 * response: the rail stayed data-ready and kept feeding
			 * an ICM tunnel that no longer consumed descriptors. Ask
			 * the existing native negotiation worker to disconnect
			 * and re-establish it once. Re-enabling the same ring does
			 * not count as recovery; only a completion clears the
			 * per-path attempt latch.
			 */
			if (tbv_path_mark_tx_recovery_attempted(path))
				tbv_native_control_recover_stalled_rail(path->rail);

			if (time_after(jiffies,
				       READ_ONCE(path->tx_last_stall_warn_jiffies) +
				       msecs_to_jiffies(tx_stall_warn_ms)) &&
			    __ratelimit(&stall_rs)) {
				pr_warn("tx stall route=0x%llx rail=0x%x inflight=%d posted=%lld completed=%lld tx_hop=%d rx_hop=%d out_hop=%d remote_out_hop=%d tx_flags=0x%x rx_flags=0x%x e2e=%u\n",
					path->rail && path->rail->peer ?
						path->rail->peer->xd->route : 0,
					path->rail ? path->rail->rail_id :
						0xffffffff,
					atomic_read(&path->tx_inflight),
					atomic64_read(&path->data_tx_posted) +
						atomic64_read(&path->control_tx_posted),
					atomic64_read(&path->data_tx_completed) +
						atomic64_read(&path->control_tx_completed),
					path->local_tx_hop, path->local_rx_hop,
					path->local_transmit_path,
					path->remote_transmit_path,
					path->cfg.tx_flags, path->cfg.rx_flags,
					path->cfg.e2e);
				WRITE_ONCE(path->tx_last_stall_warn_jiffies,
					   jiffies);
			}
		}
	}

	if (atomic_read(&path->tx_inflight) > 0 || completed)
		tbv_path_queue_tx_poll(path,
				       msecs_to_jiffies(READ_ONCE(tx_poll_delay_ms)));
	/*
	 * A queue that is stalled with nothing inflight generates neither
	 * completions nor interrupts, so this is the only thing that keeps the
	 * ceiling ticking.
	 */
	if (queued)
		tbv_path_arm_queue_watchdog(path);
}

static void tbv_path_rx_supp_poll_work(struct work_struct *work)
{
	struct tbv_path *path = container_of(to_delayed_work(work),
					     struct tbv_path,
					     rx_supp_poll_work);
	struct tb_ring *ring = READ_ONCE(path->rx_ring);
	struct ring_frame *frame;
	u64 completed = 0;

	if (!ring)
		return;

	atomic64_inc(&path->rx_supp_poll_calls);
	while ((frame = tb_ring_poll(ring))) {
		if (frame->callback)
			frame->callback(ring, frame, false);
		completed++;
	}
	if (completed)
		atomic64_add(completed, &path->rx_supp_poll_completed);

	if (completed || time_before(jiffies,
				     READ_ONCE(path->rx_supp_poll_until)))
		tbv_path_queue_delayed_work(
			path, &path->rx_supp_poll_work,
			msecs_to_jiffies(TBV_RX_SUPP_POLL_DELAY_MS));
}

static int tbv_path_post_rx_frame(struct tbv_data_frame *f);

static void tbv_path_rx_start_raw(struct tbv_path *path,
				  const struct tbv_native_data_header *hdr)
{
	path->rx_raw_opcode = hdr->opcode;
	path->rx_raw_flags = hdr->flags;
	path->rx_raw_dest_qp = hdr->dest_qp;
	path->rx_raw_src_qp = hdr->src_qp;
	path->rx_raw_psn = hdr->psn;
	path->rx_raw_imm_data = hdr->imm_data;
	path->rx_raw_rkey = hdr->rkey;
	path->rx_raw_base = hdr->remote_addr;
	path->rx_raw_frag_base = hdr->frag_offset;
	path->rx_raw_done = 0;
	path->rx_raw_remaining = hdr->length;
	path->rx_raw_pending = hdr->length != 0;
}

static void tbv_path_rx_raw_payload(struct tbv_path *path,
				    struct tbv_state *state,
				    const void *payload, u32 len)
{
	struct tbv_native_data_header stream = {};
	struct tbv_native_data_header hdr = {};
	int ret;

	if (!path->rx_raw_pending || !len || len > path->rx_raw_remaining) {
		if (state)
			atomic64_inc(&state->data_rx_bad_frame);
		path->rx_raw_pending = false;
		path->rx_raw_remaining = 0;
		return;
	}

	stream.opcode = path->rx_raw_opcode;
	stream.flags = path->rx_raw_flags;
	stream.dest_qp = path->rx_raw_dest_qp;
	stream.src_qp = path->rx_raw_src_qp;
	stream.psn = path->rx_raw_psn;
	stream.length = path->rx_raw_done + path->rx_raw_remaining;
	stream.imm_data = path->rx_raw_imm_data;
	stream.remote_addr = path->rx_raw_base;
	stream.rkey = path->rx_raw_rkey;
	stream.frag_offset = path->rx_raw_frag_base;

	ret = tbv_native_data_raw_payload_header(&stream, path->rx_raw_done,
						 path->rx_raw_remaining, len,
						 &hdr);
	if (ret) {
		if (state)
			atomic64_inc(&state->data_rx_bad_frame);
		path->rx_raw_pending = false;
		path->rx_raw_remaining = 0;
		return;
	}

	if (len == path->rx_raw_remaining)
		path->rx_raw_pending = false;
	path->rx_raw_done += len;
	path->rx_raw_remaining -= len;
	if (state)
		tbv_ibdev_rx_native_frame(state, path, &hdr, payload);
}

static void tbv_path_zcopy_tx_complete(struct tb_ring *ring,
				       struct ring_frame *frame,
				       bool canceled)
{
	struct tbv_tx_packet *packet = container_of(frame,
						   struct tbv_tx_packet,
						   frame);
	struct tbv_path *path = packet->path;
	struct tbv_state *state = tbv_path_state(path);
	unsigned long flags;

	/* Ring progress; see tbv_path_tx_complete(). */
	WRITE_ONCE(path->tx_last_progress_jiffies, jiffies);
	spin_lock_irqsave(&path->tx_lock, flags);
	if (packet->inflight) {
		list_del_init(&packet->node);
		packet->inflight = false;
	}
	path->tx_ring_stalled = false;
	path->tx_recovery_attempted = false;
	spin_unlock_irqrestore(&path->tx_lock, flags);

	if (state) {
		if (canceled)
			atomic64_inc(&state->data_tx_canceled);
		else
			atomic64_inc(&state->data_tx_completed);
	}
	if (!canceled)
		atomic64_inc(&path->data_tx_completed);

	tbv_path_finish_raw_stream_if_needed(path, packet);
	tbv_path_tx_packet_release(packet, canceled ? -ECANCELED : 0);
	atomic_dec(&path->tx_inflight);
	tbv_path_schedule_tx(path);
}

static void tbv_path_rx_complete(struct tb_ring *ring, struct ring_frame *frame,
				 bool canceled)
{
	struct tbv_data_frame *f = container_of(frame, struct tbv_data_frame,
						frame);
	struct tbv_path *path = f->path;
	struct tbv_state *state = tbv_path_state(path);
	u32 len = tbv_frame_len(frame);
	u32 return_rx_credits = 0;
	u32 add_remote_credits = 0;
	u32 sync_remote_credits = 0;
	bool have_sync_remote_credits = false;
	bool was_raw_payload;

	if (canceled)
		return;
	if (state)
		atomic64_inc(&state->data_rx_completed);
	atomic64_inc(&path->data_rx_completed);

	/*
	 * The NHI reports per-frame CRC failure and RX buffer overrun in
	 * frame->flags. Parsing such a frame reads corrupt bytes: as a header
	 * it lands in data_rx_bad_header, and mid-raw-stream it would be
	 * scattered into a user MR as payload. Drop it here and let the
	 * sender's NAK/timeout resend it. Counting the two causes separately
	 * also settles whether the residual bad-header rate is wire corruption
	 * or a software framing bug.
	 */
	if (tbv_frame_hw_error(frame->flags)) {
		if (state) {
			if (frame->flags & RING_DESC_CRC_ERROR) {
				atomic64_inc(&state->data_rx_crc_error);
				/*
				 * Localize WHICH frame class the NHI CRCs bad:
				 * mid-raw-stream (rx_raw_pending) means a zcopy
				 * PAYLOAD frame; otherwise a standalone frame (a
				 * raw-stream HEADER, a copied data frame, or a
				 * control frame -- all staged from kernel buffers).
				 * If in_stream dominates, the corruption is the
				 * zcopy payload frames specifically. maxsize splits
				 * out exactly-4096 frames (the frame-budget theory).
				 */
				if (path->rx_raw_pending)
					atomic64_inc(&state->data_rx_crc_error_in_stream);
				else
					atomic64_inc(&state->data_rx_crc_error_standalone);
				if (len >= TBV_DATA_FRAME_SIZE)
					atomic64_inc(&state->data_rx_crc_error_maxsize);
				if (!atomic_cmpxchg(&state->crc_error_reported, 0, 1))
					pr_info("first CRC-error frame: size=%u sof=%u eof=%u flags=0x%x rx_raw_pending=%d raw_op=%u raw_done=%u raw_remaining=%u\n",
						len, frame->sof, frame->eof,
						frame->flags, path->rx_raw_pending,
						path->rx_raw_opcode,
						path->rx_raw_done,
						path->rx_raw_remaining);
			} else {
				atomic64_inc(&state->data_rx_overrun);
			}
		}
		/*
		 * A corrupt frame inside a raw stream desyncs the receiver's
		 * byte counter, so abandon the stream; the sender's NAK or
		 * timeout resends the whole window.
		 */
		if (path->rx_raw_pending) {
			path->rx_raw_pending = false;
			path->rx_raw_remaining = 0;
		}
		goto repost;
	}

	dma_sync_single_for_cpu(tb_ring_dma_device(ring), f->dma,
				TBV_DATA_FRAME_SIZE, DMA_FROM_DEVICE);
	was_raw_payload = path->rx_raw_pending;
	if (len <= TBV_DATA_FRAME_SIZE && state) {
		struct tbv_native_data_header hdr;
		int ret;

		if (path->rail && path->rail->peer &&
		    path->rail->peer->backend == TBV_BACKEND_APPLE) {
			tbv_ibdev_rx_apple_frame(state, path, f->buf, len,
						 frame->sof, frame->eof);
		} else if (was_raw_payload &&
			   tbv_native_data_parse_header(f->buf, len, &hdr)) {
			return_rx_credits = 1;
			tbv_path_rx_raw_payload(path, state, f->buf, len);
		} else {
			/*
			 * A frame that parses as a valid native header cannot
			 * be raw payload unless the user data forges the TVD1
			 * magic/version/opcode (~2^-50): the sender never
			 * interleaves headers inside a stream, so seeing one
			 * mid-stream means payload frames were LOST. Dropping
			 * the desynced stream here bounds the damage to the
			 * stream (one fragment for split streams) instead of
			 * scattering later headers into user memory; the
			 * retransmit path resends the fragment.
			 */
			if (was_raw_payload) {
				atomic64_inc(&state->data_rx_bad_frame);
				path->rx_raw_pending = false;
				path->rx_raw_remaining = 0;
			}
			ret = tbv_native_data_parse_header(f->buf, len, &hdr);
			if (!ret && hdr.opcode == TBV_NATIVE_DATA_OP_PATH_CREDIT) {
				if (tbv_native_data_valid_path_credit(&hdr))
					add_remote_credits = hdr.imm_data;
				else
					atomic64_inc(&state->data_rx_bad_header);
			} else if (!ret &&
				   hdr.opcode ==
					   TBV_NATIVE_DATA_OP_PATH_CREDIT_SYNC) {
				if (tbv_native_data_valid_path_credit_sync(&hdr)) {
					sync_remote_credits = hdr.frag_offset;
					have_sync_remote_credits = true;
				} else {
					atomic64_inc(&state->data_rx_bad_header);
				}
			} else if (!ret &&
				   (hdr.flags & TBV_NATIVE_DATA_F_RAW_STREAM)) {
				if (len != TBV_NATIVE_DATA_HDR_SIZE ||
				    !hdr.length ||
				    (hdr.flags & ~(TBV_NATIVE_DATA_F_LAST |
						   TBV_NATIVE_DATA_F_SOLICITED |
						   TBV_NATIVE_DATA_F_RAW_STREAM))) {
					atomic64_inc(&state->data_rx_bad_frame);
				} else {
					return_rx_credits = 1;
					tbv_path_rx_start_raw(path, &hdr);
				}
			} else {
				if (!ret &&
				    tbv_native_data_consumes_rx_credit(hdr.opcode))
					return_rx_credits = 1;
				tbv_ibdev_rx_frame(state, path, f->buf, len);
			}
		}
	} else if (state) {
		atomic64_inc(&state->data_rx_bad_frame);
	}

	if (return_rx_credits) {
		if (state)
			atomic64_inc(&state->data_rx_credit_eligible);
		atomic64_inc(&path->data_rx_credit_eligible);
	}

repost:
	if (path->state == TBV_PATH_RING_STARTED ||
	    path->state == TBV_PATH_TUNNEL_ENABLED) {
		int ret = tbv_path_post_rx_frame(f);

		if (ret) {
			pr_warn_ratelimited("RX repost failed ret=%d\n", ret);
			if (state)
				atomic64_inc(&state->data_rx_repost_failed);
			atomic64_inc(&path->data_rx_repost_failed);
		} else {
			if (return_rx_credits)
				tbv_path_return_rx_data_credit(path,
							       return_rx_credits);
			if (add_remote_credits)
				tbv_path_add_remote_rx_credits(path,
							       add_remote_credits);
			if (have_sync_remote_credits)
				tbv_path_sync_remote_rx_credits(
					path, sync_remote_credits);
		}
	}
}

static int tbv_path_alloc_frames(struct tbv_path *path, bool tx)
{
	struct tbv_data_frame **frames_out = tx ? &path->tx_frames :
						 &path->rx_frames;
	u32 *count_out = tx ? &path->tx_frame_count : &path->rx_frame_count;
	u32 count = tx ? path->cfg.tx_ring_size : path->cfg.rx_ring_size;
	struct tb_ring *ring = tx ? path->tx_ring : path->rx_ring;
	struct device *dma_dev = tb_ring_dma_device(ring);
	struct tbv_data_frame *frames;
	int i;
	int ret = -ENOMEM;

	if (!tbv_dma_device_ready(dma_dev)) {
		pr_warn_ratelimited("%s ring DMA device is not ready for mapping\n",
				    tx ? "TX" : "RX");
		return -EPROBE_DEFER;
	}

	frames = kcalloc(count, sizeof(*frames), GFP_KERNEL);
	if (!frames)
		return -ENOMEM;

	for (i = 0; i < count; i++) {
		struct tbv_data_frame *f = &frames[i];

		f->path = path;
		f->tx = tx;
		INIT_LIST_HEAD(&f->frame.list);
		INIT_LIST_HEAD(&f->free_node);
		f->buf = kmalloc(TBV_DATA_FRAME_SIZE, GFP_KERNEL);
		if (!f->buf)
			goto err;
		f->dma = dma_map_single(dma_dev, f->buf, TBV_DATA_FRAME_SIZE,
					tx ? DMA_TO_DEVICE : DMA_FROM_DEVICE);
		if (dma_mapping_error(dma_dev, f->dma)) {
			kfree(f->buf);
			f->buf = NULL;
			ret = -EIO;
			goto err;
		}
		f->frame.buffer_phy = f->dma;
		f->frame.size = 0;
		if (tx)
			list_add_tail(&f->free_node, &path->tx_free);
	}

	*frames_out = frames;
	*count_out = count;
	return 0;

err:
	while (--i >= 0) {
		struct tbv_data_frame *f = &frames[i];

		if (f->buf) {
			dma_unmap_single(dma_dev, f->dma, TBV_DATA_FRAME_SIZE,
					 tx ? DMA_TO_DEVICE : DMA_FROM_DEVICE);
			kfree(f->buf);
		}
	}
	kfree(frames);
	return ret;
}

static void tbv_path_free_frames(struct tbv_path *path, bool tx)
{
	struct tbv_data_frame *frames = tx ? path->tx_frames : path->rx_frames;
	u32 count = tx ? path->tx_frame_count : path->rx_frame_count;
	struct tb_ring *ring = tx ? path->tx_ring : path->rx_ring;
	struct device *dma_dev;
	u32 i;

	if (!frames || !ring)
		return;

	dma_dev = tb_ring_dma_device(ring);
	if (!tbv_dma_device_ready(dma_dev)) {
		pr_warn_ratelimited("%s ring DMA device is not ready for unmapping\n",
				    tx ? "TX" : "RX");
		dma_dev = NULL;
	}
	for (i = 0; i < count; i++) {
		struct tbv_data_frame *f = &frames[i];

		if (!f->buf)
			continue;
		if (dma_dev)
			dma_unmap_single(dma_dev, f->dma, TBV_DATA_FRAME_SIZE,
					 tx ? DMA_TO_DEVICE : DMA_FROM_DEVICE);
		kfree(f->buf);
	}

	if (tx) {
		path->tx_frames = NULL;
		path->tx_frame_count = 0;
		INIT_LIST_HEAD(&path->tx_free);
	} else {
		path->rx_frames = NULL;
		path->rx_frame_count = 0;
	}
	kfree(frames);
}

static int tbv_path_alloc_control_packets(struct tbv_path *path)
{
	struct tbv_tx_packet *packets;
	u32 count = tbv_path_control_packet_count(path);
	u32 i;

	packets = kcalloc(count, sizeof(*packets), GFP_KERNEL);
	if (!packets)
		return -ENOMEM;

	for (i = 0; i < count; i++) {
		struct tbv_tx_packet *packet = &packets[i];

		INIT_LIST_HEAD(&packet->node);
		packet->path = path;
		packet->buf = packet->control_buf;
		packet->control = true;
		packet->pooled = true;
		list_add_tail(&packet->node, &path->tx_control_free);
	}

	path->tx_control_packets = packets;
	path->tx_control_packet_count = count;
	/*
	 * The queue remains bounded, but its bound must cover at least one WR
	 * of the maximum message size query_port advertises. Otherwise that WR
	 * can never satisfy tbv_path_reserve_data(): deferring it merely turns
	 * a synchronous -ENOMEM into an eventual asynchronous timeout.
	 */
	path->tx_data_queue_limit =
		max_t(u32, path->cfg.tx_ring_size * TBV_DATA_QUEUE_MULTIPLIER,
		      TBV_NATIVE_DATA_MAX_FRAGS);
	return 0;
}

static void tbv_path_free_control_packets(struct tbv_path *path)
{
	kfree(path->tx_control_packets);
	path->tx_control_packets = NULL;
	path->tx_control_packet_count = 0;
	path->tx_control_queued = 0;
	path->tx_data_queued = 0;
	path->tx_data_reserved = 0;
	path->tx_data_queue_limit = 0;
	INIT_LIST_HEAD(&path->tx_control_free);
}

#if IS_ENABLED(CONFIG_KUNIT)
int tbv_test_max_message_frame_capacity(u32 *required_frames_out,
					u32 *queue_limit_out)
{
	struct tbv_path path = {};
	int ret;

	if (!required_frames_out || !queue_limit_out)
		return -EINVAL;

	path.cfg.tx_ring_size = 16;
	INIT_LIST_HEAD(&path.tx_control_free);
	ret = tbv_path_alloc_control_packets(&path);
	if (ret)
		return ret;

	*required_frames_out = TBV_NATIVE_DATA_MAX_FRAGS;
	*queue_limit_out = path.tx_data_queue_limit;
	tbv_path_free_control_packets(&path);
	return 0;
}
#endif

static int tbv_path_alloc_data_packets(struct tbv_path *path)
{
	struct tbv_tx_packet *packets;
	u32 count = tbv_path_data_packet_count(path);
	u32 i;

	if (!path->rail || !path->rail->peer ||
	    path->rail->peer->backend != TBV_BACKEND_APPLE)
		return 0;

	packets = kcalloc(count, sizeof(*packets), GFP_KERNEL);
	if (!packets)
		return -ENOMEM;

	for (i = 0; i < count; i++) {
		struct tbv_tx_packet *packet = &packets[i];

		INIT_LIST_HEAD(&packet->node);
		packet->path = path;
		packet->buf = kmalloc(TBV_DATA_FRAME_SIZE, GFP_KERNEL);
		if (!packet->buf)
			goto err;
		packet->pooled = true;
		list_add_tail(&packet->node, &path->tx_data_free);
	}

	path->tx_data_packets = packets;
	path->tx_data_packet_count = count;
	return 0;

err:
	while (i-- > 0)
		kfree(packets[i].buf);
	kfree(packets);
	INIT_LIST_HEAD(&path->tx_data_free);
	return -ENOMEM;
}

static void tbv_path_free_data_packets(struct tbv_path *path)
{
	u32 i;

	for (i = 0; i < path->tx_data_packet_count; i++)
		kfree(path->tx_data_packets[i].buf);
	kfree(path->tx_data_packets);
	path->tx_data_packets = NULL;
	path->tx_data_packet_count = 0;
	INIT_LIST_HEAD(&path->tx_data_free);
}

static int tbv_path_post_rx_frame(struct tbv_data_frame *f)
{
	struct tbv_path *path = f->path;

	f->frame.callback = tbv_path_rx_complete;
	f->frame.size = 0;
	f->frame.flags = 0;
	f->frame.sof = 0;
	f->frame.eof = 0;
	dma_sync_single_for_device(tb_ring_dma_device(path->rx_ring), f->dma,
				   TBV_DATA_FRAME_SIZE, DMA_FROM_DEVICE);
	return tb_ring_rx(path->rx_ring, &f->frame);
}

const char *tbv_path_state_name(enum tbv_path_state state)
{
	switch (state) {
	case TBV_PATH_NEW:
		return "new";
	case TBV_PATH_RING_ALLOCATED:
		return "ring_allocated";
	case TBV_PATH_RING_STARTED:
		return "ring_started";
	case TBV_PATH_TUNNEL_ENABLED:
		return "tunnel_enabled";
	case TBV_PATH_STOPPED:
		return "stopped";
	default:
		return "unknown";
	}
}

int tbv_path_alloc_rings(struct tbv_path *path, struct tb_xdomain *xd,
			 int requested_transmit_path)
{
	int e2e_tx_hop = 0;
	int transmit_path;
	int tx_hop;
	int rx_hop;
	int ret;

	if (path->state != TBV_PATH_NEW && path->state != TBV_PATH_STOPPED)
		return -EBUSY;

	tx_hop = path->cfg.tx_hop;
	rx_hop = path->cfg.rx_hop;
	if (requested_transmit_path < 0)
		requested_transmit_path = path->cfg.transmit_path;

	if (path->cfg.receive_path >= 0) {
		ret = tb_xdomain_alloc_in_hopid(xd, path->cfg.receive_path);
		if (ret != path->cfg.receive_path) {
			if (ret >= 0)
				tb_xdomain_release_in_hopid(xd, ret);
			return ret < 0 ? ret : -EBUSY;
		}
		path->remote_transmit_path = ret;
	}

	path->tx_ring = tb_ring_alloc_tx(xd->tb->nhi, tx_hop,
					 path->cfg.tx_ring_size,
					 path->cfg.tx_flags);
	if (!path->tx_ring) {
		ret = -ENOMEM;
		goto err_in_hop;
	}

	transmit_path = tb_xdomain_alloc_out_hopid(xd,
						   requested_transmit_path);
	if (transmit_path < 0) {
		ret = transmit_path;
		goto err_tx_ring;
	}
	path->local_transmit_path = transmit_path;
	path->local_tx_hop = path->tx_ring->hop;

	if (path->cfg.e2e)
		e2e_tx_hop = path->tx_ring->hop;

	path->tx_poll_enabled = tbv_path_progress_poll_enabled(path);
	/*
	 * RX frames for the Apple-compatible verbs path carry no per-message
	 * sequence number. Processing the same RX ring from the normal
	 * completion path and a supplemental poller can therefore expose later
	 * frames to the verbs receive queue before earlier frames. Keep RX
	 * completion single-sourced; TX polling is still used for timely send
	 * completions.
	 */
	path->rx_supp_poll_enabled = false;
	path->rx_ring = tb_ring_alloc_rx(xd->tb->nhi, rx_hop,
					 path->cfg.rx_ring_size,
					 path->cfg.rx_flags, e2e_tx_hop,
					 path->cfg.sof_mask,
					 path->cfg.eof_mask,
					 NULL, NULL);
	if (!path->rx_ring) {
		ret = -ENOMEM;
		goto err_out_hop;
	}
	path->local_rx_hop = path->rx_ring->hop;

	ret = tbv_path_configure_ring_throttling(path);
	if (ret)
		goto err_rx_ring;
	ret = tbv_path_alloc_frames(path, true);
	if (ret)
		goto err_rx_ring;
	ret = tbv_path_alloc_frames(path, false);
	if (ret)
		goto err_tx_frames;
	ret = tbv_path_alloc_control_packets(path);
	if (ret)
		goto err_rx_frames;
	ret = tbv_path_alloc_data_packets(path);
	if (ret)
		goto err_control_packets;

	path->state = TBV_PATH_RING_ALLOCATED;
	return 0;

err_control_packets:
	tbv_path_free_control_packets(path);
err_rx_frames:
	tbv_path_free_frames(path, false);
err_tx_frames:
	tbv_path_free_frames(path, true);
err_rx_ring:
	tb_ring_free(path->rx_ring);
	path->rx_ring = NULL;
	path->local_rx_hop = -1;
err_out_hop:
	tb_xdomain_release_out_hopid(xd, path->local_transmit_path);
	path->local_transmit_path = -1;
err_tx_ring:
	tb_ring_free(path->tx_ring);
	path->tx_ring = NULL;
	path->local_tx_hop = -1;
err_in_hop:
	if (path->remote_transmit_path >= 0) {
		tb_xdomain_release_in_hopid(xd, path->remote_transmit_path);
		path->remote_transmit_path = -1;
	}
	return ret;
}

int tbv_path_start_rings(struct tbv_path *path)
{
	u32 i;
	int ret;

	if (path->state != TBV_PATH_RING_ALLOCATED)
		return -EINVAL;

	tb_ring_start(path->tx_ring);
	tb_ring_start(path->rx_ring);
	path->state = TBV_PATH_RING_STARTED;
	for (i = 0; i < path->rx_frame_count; i++) {
		ret = tbv_path_post_rx_frame(&path->rx_frames[i]);
		if (ret) {
			pr_warn("post RX frame %u/%u failed ret=%d\n", i,
				path->rx_frame_count, ret);
			return ret;
		}
	}
	return 0;
}

int tbv_path_enable_tunnel(struct tbv_path *path, struct tb_xdomain *xd,
			   int remote_transmit_path)
{
	bool in_hop_allocated = false;
	int ret;

	if (path->state != TBV_PATH_RING_STARTED)
		return -EINVAL;

	if (path->remote_transmit_path >= 0) {
		if (path->remote_transmit_path != remote_transmit_path)
			return -EBUSY;
	} else {
		ret = tb_xdomain_alloc_in_hopid(xd, remote_transmit_path);
		if (ret != remote_transmit_path) {
			if (ret >= 0)
				tb_xdomain_release_in_hopid(xd, ret);
			return ret < 0 ? ret : -EBUSY;
		}
		path->remote_transmit_path = ret;
		in_hop_allocated = true;
	}

	ret = tb_xdomain_enable_paths(xd, path->local_transmit_path,
				      path->local_tx_hop,
				      remote_transmit_path,
				      path->local_rx_hop);
	if (ret) {
		tb_xdomain_release_in_hopid(xd, path->remote_transmit_path);
		path->remote_transmit_path = -1;
		return ret;
	}

	if (!in_hop_allocated)
		path->remote_transmit_path = remote_transmit_path;
	path->state = TBV_PATH_TUNNEL_ENABLED;
	WRITE_ONCE(path->tx_last_progress_jiffies, jiffies);
	pr_info("enabled tunnel route=0x%llx rail=0x%x out_hop=%d remote_out_hop=%d tx_hop=%d rx_hop=%d tx_flags=0x%x rx_flags=0x%x e2e=%u\n",
		path->rail && path->rail->peer ? path->rail->peer->xd->route : 0,
		path->rail ? path->rail->rail_id : 0xffffffff,
		path->local_transmit_path, remote_transmit_path,
		path->local_tx_hop, path->local_rx_hop,
		path->cfg.tx_flags, path->cfg.rx_flags, path->cfg.e2e);
	tbv_path_arm_credit_sync(path);
	tbv_path_schedule_tx(path);
	return 0;
}

/*
 * Disable an enabled tunnel but KEEP the rings started, returning the path to
 * TBV_PATH_RING_STARTED so tbv_path_enable_tunnel() can re-run with a different
 * peer out-hop. Used when a peer soft-reloads and re-HELLOs with a changed
 * transmit_path (native_control.c re-HELLO supersede): the live tunnel points at
 * a hop the peer no longer owns, and enable_tunnel() would otherwise reject the
 * new hop with -EBUSY (this releases the in-hopid so the realloc succeeds). The
 * disable mirrors tbv_path_destroy()'s tunnel-teardown branch.
 */
int tbv_path_disable_tunnel(struct tbv_path *path, struct tb_xdomain *xd)
{
	int ret;

	if (path->state != TBV_PATH_TUNNEL_ENABLED)
		return -EINVAL;

	if (destroy_disable_paths) {
		ret = tb_xdomain_disable_paths(xd, path->local_transmit_path,
					      path->local_tx_hop,
					      path->remote_transmit_path,
					      path->local_rx_hop);
		if (ret)
			return ret;
	}
	if (path->remote_transmit_path >= 0) {
		tb_xdomain_release_in_hopid(xd, path->remote_transmit_path);
		path->remote_transmit_path = -1;
	}
	path->state = TBV_PATH_RING_STARTED;
	pr_info("disabled tunnel for rehop route=0x%llx rail=0x%x out_hop=%d tx_hop=%d rx_hop=%d\n",
		path->rail && path->rail->peer ? path->rail->peer->xd->route : 0,
		path->rail ? path->rail->rail_id : 0xffffffff,
		path->local_transmit_path, path->local_tx_hop, path->local_rx_hop);
	return 0;
}

static struct tbv_tx_packet *
tbv_path_alloc_data_packet_owned(struct tbv_path *path, u8 *buf, u32 len,
				 tbv_path_tx_done_fn done, void *done_ctx)
{
	struct tbv_tx_packet *packet;

	packet = kzalloc(sizeof(*packet), GFP_KERNEL);
	if (!packet)
		return NULL;

	INIT_LIST_HEAD(&packet->node);
	packet->path = path;
	packet->buf = buf;
	packet->len = len;
	packet->done = done;
	packet->done_ctx = done_ctx;
	packet->owner_ctx = done_ctx;
	packet->sof = TBV_DATA_PDF_FRAME_START;
	packet->eof = TBV_DATA_PDF_FRAME_END;
	return packet;
}

/*
 * Keep copied frame bytes in the packet allocation itself. The native framed
 * path used to allocate a payload, an ownership wrapper and then a packet for
 * every fragment. The wrapper carried no state after preparation and the
 * separate payload forced a second allocation solely so packet release could
 * free it. One allocation has the same immutable-until-completion lifetime.
 */
static struct tbv_tx_packet *
tbv_path_alloc_inline_data_packet(struct tbv_path *path, u32 len,
				  tbv_path_tx_done_fn done, void *done_ctx)
{
	struct tbv_tx_packet *packet;

	packet = kzalloc(sizeof(*packet) + len, GFP_KERNEL);
	if (!packet)
		return NULL;

	INIT_LIST_HEAD(&packet->node);
	packet->path = path;
	packet->buf = (u8 *)(packet + 1);
	packet->len = len;
	packet->done = done;
	packet->done_ctx = done_ctx;
	packet->owner_ctx = done_ctx;
	packet->sof = TBV_DATA_PDF_FRAME_START;
	packet->eof = TBV_DATA_PDF_FRAME_END;
	packet->inline_buf = true;
	return packet;
}

static struct tbv_tx_packet *
tbv_path_alloc_data_packet(struct tbv_path *path, const void *data, u32 len,
			   tbv_path_tx_done_fn done, void *done_ctx)
{
	struct tbv_tx_packet *packet;
	u8 *buf;

	buf = kmemdup(data, len, GFP_KERNEL);
	if (!buf)
		return NULL;

	packet = tbv_path_alloc_data_packet_owned(path, buf, len, done,
						  done_ctx);
	if (!packet)
		kfree(buf);
	return packet;
}

static struct tbv_tx_packet *tbv_path_alloc_pooled_data_packet(
	struct tbv_path *path, u32 len, tbv_path_tx_done_fn done, void *done_ctx)
{
	struct tbv_tx_packet *packet;
	unsigned long flags;

	spin_lock_irqsave(&path->tx_lock, flags);
	if (list_empty(&path->tx_data_free)) {
		spin_unlock_irqrestore(&path->tx_lock, flags);
		return NULL;
	}

	packet = list_first_entry(&path->tx_data_free, struct tbv_tx_packet,
				  node);
	list_del_init(&packet->node);
	spin_unlock_irqrestore(&path->tx_lock, flags);

	packet->len = len;
	packet->done = done;
	packet->done_ctx = done_ctx;
	packet->owner_ctx = done_ctx;
	packet->sof = TBV_DATA_PDF_FRAME_START;
	packet->eof = TBV_DATA_PDF_FRAME_END;
	packet->control = false;
	packet->queued = false;
	packet->zcopy = false;
	packet->unmap_dma = false;
	packet->raw_stream_start = false;
	packet->raw_stream_end = false;
	packet->raw_stream_counted = false;
	packet->start_credit_group_frames = 0;
	return packet;
}

static struct tbv_tx_packet *
tbv_path_alloc_zcopy_packet(struct tbv_path *path, dma_addr_t dma, u32 len,
			    u32 dma_len, bool unmap_dma,
			    tbv_path_tx_done_fn done, void *done_ctx)
{
	struct tbv_tx_packet *packet;

	packet = kzalloc(sizeof(*packet), GFP_KERNEL);
	if (!packet)
		return NULL;

	INIT_LIST_HEAD(&packet->node);
	INIT_LIST_HEAD(&packet->frame.list);
	packet->path = path;
	packet->len = len;
	packet->dma = dma;
	packet->dma_len = dma_len;
	packet->done = done;
	packet->done_ctx = done_ctx;
	packet->owner_ctx = done_ctx;
	packet->sof = TBV_DATA_PDF_FRAME_START;
	packet->eof = TBV_DATA_PDF_FRAME_END;
	packet->zcopy = true;
	packet->unmap_dma = unmap_dma;
	packet->raw_stream_counted = false;
	return packet;
}

static int tbv_path_enqueue_control(struct tbv_path *path, const void *data,
				    u32 len, tbv_path_tx_done_fn done,
				    void *done_ctx)
{
	struct tbv_tx_packet *packet;
	unsigned long flags;

	if (len > TBV_CONTROL_FRAME_SIZE)
		return -EMSGSIZE;

	spin_lock_irqsave(&path->tx_lock, flags);
	if (path->state != TBV_PATH_TUNNEL_ENABLED) {
		spin_unlock_irqrestore(&path->tx_lock, flags);
		return -ENOTCONN;
	}
	if (list_empty(&path->tx_control_free)) {
		spin_unlock_irqrestore(&path->tx_lock, flags);
		return -ENOMEM;
	} else {
		packet = list_first_entry(&path->tx_control_free,
					  struct tbv_tx_packet, node);
		list_del_init(&packet->node);
	}

	packet->len = len;
	packet->done = done;
	packet->done_ctx = done_ctx;
	packet->owner_ctx = done_ctx;
	packet->sof = TBV_DATA_PDF_FRAME_START;
	packet->eof = TBV_DATA_PDF_FRAME_END;
	packet->pooled = true;
	packet->queued_jiffies = jiffies;
	packet->queued = true;
	memcpy(packet->buf, data, len);
	list_add_tail(&packet->node, &path->tx_control_queue);
	path->tx_control_queued++;
	atomic64_inc(&path->control_tx_enqueued);
	spin_unlock_irqrestore(&path->tx_lock, flags);

	tbv_path_schedule_tx(path);
	return 0;
}

static int tbv_path_enqueue_data(struct tbv_path *path,
				 struct tbv_tx_packet *packet,
				 bool defer_schedule)
{
	unsigned long flags;

	spin_lock_irqsave(&path->tx_lock, flags);
	if (path->state != TBV_PATH_TUNNEL_ENABLED) {
		spin_unlock_irqrestore(&path->tx_lock, flags);
		return -ENOTCONN;
	}
	if (path->tx_data_reserved) {
		path->tx_data_reserved--;
	} else if (path->tx_data_queued >= path->tx_data_queue_limit) {
		spin_unlock_irqrestore(&path->tx_lock, flags);
		return -ENOMEM;
	}

	packet->start_credit_group_frames = 1;
	/*
	 * Enqueue time is the ceiling's reference point (tbv_path_expire_tx_
	 * data_queue). Only the release path clears it, so it measures the
	 * whole wait including a requeue after a failed post.
	 */
	packet->queued_jiffies = jiffies;
	packet->queued = true;
	list_add_tail(&packet->node, &path->tx_data_queue);
	path->tx_data_queued++;
	atomic64_inc(&path->data_tx_enqueued);
	spin_unlock_irqrestore(&path->tx_lock, flags);

	tbv_path_arm_queue_watchdog(path);
	if (!defer_schedule)
		tbv_path_schedule_tx(path);
	return 0;
}

/*
 * Put recovery data ahead of unsent initial traffic. If an unframed window is
 * already open, its remaining frames must stay at the head: inserting a
 * foreign frame between the raw header and payload would desynchronize the
 * receiver, while hiding the active payload behind the retry would deadlock
 * the scheduler's owner gate. In that case, preempt at the end of the current
 * window instead.
 */
static void tbv_path_queue_data_list_locked(struct tbv_path *path,
					    struct list_head *packets,
					    bool priority)
{
	struct tbv_tx_packet *packet;

	if (!priority) {
		list_splice_tail_init(packets, &path->tx_data_queue);
		return;
	}

	if (path->tx_raw_stream_active && path->tx_raw_stream_window_open) {
		list_for_each_entry(packet, &path->tx_data_queue, node) {
			if (packet->owner_ctx != path->tx_raw_stream_owner)
				continue;
			if (!packet->raw_stream_end)
				continue;
			list_splice_init(packets, &packet->node);
			return;
		}

		/*
		 * The open window's end must already be queued. Preserve the
		 * existing queue if state is transiently inconsistent instead
		 * of placing a foreign frame at its head.
		 */
		list_splice_tail_init(packets, &path->tx_data_queue);
		return;
	}

	list_splice_init(packets, &path->tx_data_queue);
}

static int tbv_path_enqueue_data_list(struct tbv_path *path,
				      struct list_head *packets, u32 count,
				      bool defer_schedule, bool priority)
{
	struct tbv_tx_packet *packet;
	unsigned long flags;
	bool first = true;
	u32 used;

	if (!count)
		return 0;

	spin_lock_irqsave(&path->tx_lock, flags);
	if (path->state != TBV_PATH_TUNNEL_ENABLED) {
		spin_unlock_irqrestore(&path->tx_lock, flags);
		return -ENOTCONN;
	}

	used = path->tx_data_queued + path->tx_data_reserved;
	if (count > path->tx_data_queue_limit ||
	    used > path->tx_data_queue_limit - count) {
		spin_unlock_irqrestore(&path->tx_lock, flags);
		return -ENOMEM;
	}

	list_for_each_entry(packet, packets, node) {
		packet->start_credit_group_frames = first ? count : 0;
		packet->queued_jiffies = jiffies;
		packet->queued = true;
		path->tx_data_queued++;
		atomic64_inc(&path->data_tx_enqueued);
		first = false;
	}
	tbv_path_queue_data_list_locked(path, packets, priority);
	spin_unlock_irqrestore(&path->tx_lock, flags);

	tbv_path_arm_queue_watchdog(path);
	if (!defer_schedule)
		tbv_path_schedule_tx(path);
	return 0;
}

static int tbv_path_enqueue_reserved_data_list(struct tbv_path *path,
					       struct list_head *packets,
					       u32 count, bool defer_schedule,
					       bool priority)
{
	struct tbv_tx_packet *packet;
	unsigned long flags;
	bool first = true;

	if (!count)
		return 0;

	spin_lock_irqsave(&path->tx_lock, flags);
	if (path->state != TBV_PATH_TUNNEL_ENABLED) {
		spin_unlock_irqrestore(&path->tx_lock, flags);
		return -ENOTCONN;
	}
	if (path->tx_data_reserved < count) {
		spin_unlock_irqrestore(&path->tx_lock, flags);
		return -ENOMEM;
	}

	path->tx_data_reserved -= count;
	list_for_each_entry(packet, packets, node) {
		packet->start_credit_group_frames = first ? count : 0;
		packet->queued_jiffies = jiffies;
		packet->queued = true;
		path->tx_data_queued++;
		atomic64_inc(&path->data_tx_enqueued);
		first = false;
	}
	tbv_path_queue_data_list_locked(path, packets, priority);
	spin_unlock_irqrestore(&path->tx_lock, flags);

	tbv_path_arm_queue_watchdog(path);
	if (!defer_schedule)
		tbv_path_schedule_tx(path);
	return 0;
}

int tbv_path_reserve_data(struct tbv_path *path, u32 frames)
{
	unsigned long flags;
	u32 used;

	if (!frames)
		return 0;

	spin_lock_irqsave(&path->tx_lock, flags);
	if (path->state != TBV_PATH_TUNNEL_ENABLED) {
		spin_unlock_irqrestore(&path->tx_lock, flags);
		return -ENOTCONN;
	}

	used = path->tx_data_queued + path->tx_data_reserved;
	if (frames > path->tx_data_queue_limit ||
	    used > path->tx_data_queue_limit - frames) {
		spin_unlock_irqrestore(&path->tx_lock, flags);
		return -ENOMEM;
	}

	path->tx_data_reserved += frames;
	spin_unlock_irqrestore(&path->tx_lock, flags);
	return 0;
}

void tbv_path_release_data_reservation(struct tbv_path *path, u32 frames)
{
	unsigned long flags;

	if (!frames)
		return;

	spin_lock_irqsave(&path->tx_lock, flags);
	if (path->tx_data_reserved >= frames)
		path->tx_data_reserved -= frames;
	else
		path->tx_data_reserved = 0;
	spin_unlock_irqrestore(&path->tx_lock, flags);
}

static bool tbv_path_scheduler_wake_needed(enum tbv_path_state state)
{
	return state != TBV_PATH_TUNNEL_ENABLED;
}

static void tbv_path_tx_scheduling_done_locked(struct tbv_path *path)
{
	bool wake = tbv_path_scheduler_wake_needed(path->state);

	path->tx_scheduling = false;
	if (wake)
		wake_up_all(&path->tx_idle_wait);
}

static void tbv_path_schedule_tx(struct tbv_path *path)
{
	struct tbv_state *state = tbv_path_state(path);
	unsigned long flags;

	spin_lock_irqsave(&path->tx_lock, flags);
	if (path->tx_scheduling) {
		spin_unlock_irqrestore(&path->tx_lock, flags);
		return;
	}
	path->tx_scheduling = true;
	spin_unlock_irqrestore(&path->tx_lock, flags);

	for (;;) {
		struct tbv_tx_packet *packet;
		struct tbv_data_frame *f;
		bool needs_staging;
		bool pad_short_native;
		bool old_raw_stream_active;
		bool old_raw_stream_end_seen;
		bool old_raw_stream_window_open;
		void *old_raw_stream_owner;
		u32 old_raw_stream_inflight;
		bool charged_data_credit;
		bool from_control_queue;
		bool packet_control;
		u32 old_start_credit_group_frames;
		int ret;

		spin_lock_irqsave(&path->tx_lock, flags);
		if (path->state != TBV_PATH_TUNNEL_ENABLED ||
		    (list_empty(&path->tx_control_queue) &&
		     list_empty(&path->tx_data_queue))) {
			tbv_path_tx_scheduling_done_locked(path);
			spin_unlock_irqrestore(&path->tx_lock, flags);
			return;
		}

		/*
		 * Gate on the OPEN unframed window (header posted, end not
		 * yet), not on stream-active: between chained per-fragment
		 * windows the ring order is safe again, so control frames
		 * (ACKs) and other QPs' data interleave at fragment
		 * granularity instead of stalling behind a whole message.
		 */
		if (path->tx_raw_stream_active &&
		    path->tx_raw_stream_window_open) {
			if (list_empty(&path->tx_data_queue)) {
				tbv_path_tx_scheduling_done_locked(path);
				spin_unlock_irqrestore(&path->tx_lock, flags);
				return;
			}
			packet = list_first_entry(&path->tx_data_queue,
						  struct tbv_tx_packet, node);
			/*
			 * While a window is open only its own payload frames may
			 * go out (a foreign frame between header and payload
			 * would desync the receiver's raw parser). A payload
			 * frame is any owner-matched packet that is not the next
			 * header; it may be zcopy (DMA from the MR / persistent
			 * mapping) OR staged (a kernel copy, zcopy_stage_payload)
			 * -- staged ones still need a ring frame, so key
			 * needs_staging off zcopy, not a hardcoded false.
			 */
			if (packet->raw_stream_start ||
			    packet->owner_ctx != path->tx_raw_stream_owner) {
				tbv_path_tx_scheduling_done_locked(path);
				spin_unlock_irqrestore(&path->tx_lock, flags);
				return;
			}
			from_control_queue = false;
			needs_staging = !packet->zcopy;
		} else if (!list_empty(&path->tx_control_queue)) {
			packet = list_first_entry(&path->tx_control_queue,
						  struct tbv_tx_packet, node);
			from_control_queue = true;
			needs_staging = true;
		} else {
			if (list_empty(&path->tx_data_queue)) {
				tbv_path_tx_scheduling_done_locked(path);
				spin_unlock_irqrestore(&path->tx_lock, flags);
				return;
			}
			packet = list_first_entry(&path->tx_data_queue,
						  struct tbv_tx_packet, node);
			from_control_queue = false;
			needs_staging = !packet->zcopy;
		}

		/*
		 * Data and reliability/control frames share one hardware ring,
		 * so the in-flight ceiling is a ring budget, not a data-only
		 * budget. Control retains queue priority above, but may no
		 * longer turn one loss into hundreds of outstanding
		 * descriptors while the data side is already at its limit.
		 */
		if (!tbv_path_tx_inflight_available(
			    atomic_read(&path->tx_inflight),
			    READ_ONCE(data_tx_max_inflight))) {
			tbv_path_tx_scheduling_done_locked(path);
			spin_unlock_irqrestore(&path->tx_lock, flags);
			return;
		}

		if (needs_staging && list_empty(&path->tx_free)) {
			tbv_path_tx_scheduling_done_locked(path);
			spin_unlock_irqrestore(&path->tx_lock, flags);
			return;
		}
		if (needs_staging && !from_control_queue) {
			u32 reserve = tbv_path_tx_control_frame_reserve(path);
			/*
			 * Clamp: a negative counter read into a u32 becomes
			 * ~4 billion, which makes available 0 and refuses every
			 * staged frame for the life of the path.
			 */
			u32 inflight = max(atomic_read(&path->tx_inflight), 0);
			u32 available = path->tx_frame_count > inflight ?
					path->tx_frame_count - inflight : 0;

			if (available <= reserve) {
				tbv_path_tx_scheduling_done_locked(path);
				spin_unlock_irqrestore(&path->tx_lock, flags);
				return;
			}
		}

		charged_data_credit = false;
		old_start_credit_group_frames = packet->start_credit_group_frames;
		if (!packet->control && path->tx_remote_data_credit_max) {
			u32 start_credit_required =
				tbv_native_data_start_credit_required(
					packet->start_credit_group_frames,
					path->tx_remote_data_credit_max);

			if (path->tx_remote_data_credits <
			    max_t(u32, 1, start_credit_required)) {
				if (state)
					atomic64_inc(&state->data_tx_credit_stalls);
				atomic64_inc(&path->data_tx_credit_stalls);
				tbv_path_tx_scheduling_done_locked(path);
				spin_unlock_irqrestore(&path->tx_lock, flags);
				/*
				 * Credits only return on peer PATH_CREDIT
				 * frames; if none come this branch is the last
				 * code that ever runs for these packets, so the
				 * ceiling has to be ticking from here.
				 */
				tbv_path_arm_queue_watchdog(path);
				return;
			}
			path->tx_remote_data_credits--;
			charged_data_credit = true;
			if (state)
				atomic64_inc(&state->data_tx_credit_consumed);
			atomic64_inc(&path->data_tx_credit_consumed);
		}
		packet->start_credit_group_frames = 0;

		if (from_control_queue)
			path->tx_control_queued--;
		else
			path->tx_data_queued--;
		list_del_init(&packet->node);
		packet->queued = false;
		packet_control = packet->control;
		old_raw_stream_active = path->tx_raw_stream_active;
		old_raw_stream_owner = path->tx_raw_stream_owner;
		old_raw_stream_end_seen = path->tx_raw_stream_end_seen;
		old_raw_stream_window_open = path->tx_raw_stream_window_open;
		old_raw_stream_inflight = path->tx_raw_stream_inflight;
		tbv_path_count_raw_stream_locked(path, packet);

		if (needs_staging) {
			f = list_first_entry(&path->tx_free,
					     struct tbv_data_frame, free_node);
			list_del_init(&f->free_node);
		} else {
			f = NULL;
		}
		atomic_inc(&path->tx_inflight);
		spin_unlock_irqrestore(&path->tx_lock, flags);

		if (packet->zcopy) {
			packet->frame.buffer_phy = packet->dma;
			packet->frame.callback = tbv_path_zcopy_tx_complete;
			packet->frame.size = packet->len == TBV_DATA_FRAME_SIZE ?
					     0 : packet->len;
			packet->frame.flags = 0;
			packet->frame.sof = packet->sof;
			packet->frame.eof = packet->eof;

			spin_lock_irqsave(&path->tx_lock, flags);
			list_add_tail(&packet->node, &path->tx_zcopy_inflight);
			packet->inflight = true;
			spin_unlock_irqrestore(&path->tx_lock, flags);

			ret = tb_ring_tx(path->tx_ring, &packet->frame);
			if (!ret) {
				if (state)
					atomic64_inc(&state->data_tx_posted);
				atomic64_inc(&path->data_tx_posted);
				tbv_path_queue_tx_poll(path, 0);
				tbv_path_queue_rx_supp_poll(
					path,
					msecs_to_jiffies(
						TBV_RX_SUPP_POLL_DELAY_MS));
				continue;
			}

			if (state)
				atomic64_inc(&state->data_tx_errors);
			spin_lock_irqsave(&path->tx_lock, flags);
			if (packet->inflight) {
				list_del_init(&packet->node);
				packet->inflight = false;
			}
			path->tx_raw_stream_active = old_raw_stream_active;
			path->tx_raw_stream_owner = old_raw_stream_owner;
			path->tx_raw_stream_end_seen = old_raw_stream_end_seen;
			path->tx_raw_stream_window_open =
				old_raw_stream_window_open;
			path->tx_raw_stream_inflight = old_raw_stream_inflight;
			packet->raw_stream_counted = false;
			if (charged_data_credit) {
				if (path->tx_remote_data_credits <
				    path->tx_remote_data_credit_max)
					path->tx_remote_data_credits++;
			}
			if (ret == -ENOMEM &&
			    path->state == TBV_PATH_TUNNEL_ENABLED &&
			    (packet->done || packet->owner_ctx)) {
				list_add(&packet->node, &path->tx_data_queue);
				path->tx_data_queued++;
				packet->queued = true;
				packet->start_credit_group_frames =
					old_start_credit_group_frames;
				tbv_path_tx_scheduling_done_locked(path);
				spin_unlock_irqrestore(&path->tx_lock, flags);
				atomic_dec(&path->tx_inflight);
				return;
			}
			spin_unlock_irqrestore(&path->tx_lock, flags);
			atomic_dec(&path->tx_inflight);
			tbv_path_finish_raw_stream_if_needed(path, packet);
			tbv_path_tx_packet_release(packet, ret);
			spin_lock_irqsave(&path->tx_lock, flags);
			tbv_path_tx_scheduling_done_locked(path);
			spin_unlock_irqrestore(&path->tx_lock, flags);
			return;
		}

		pad_short_native =
			READ_ONCE(native_pad_short_frames) &&
			path->rail && path->rail->peer &&
			path->rail->peer->backend == TBV_BACKEND_NATIVE &&
			tbv_native_data_can_pad_short_frame(packet->buf,
							    packet->len);
		memcpy(f->buf, packet->buf, packet->len);
		if (pad_short_native)
			memset(f->buf + packet->len, 0,
			       TBV_DATA_FRAME_SIZE - packet->len);
		f->packet = packet;
		f->done = packet->done;
		f->done_ctx = packet->done_ctx;
		f->frame.callback = tbv_path_tx_complete;
		f->frame.size = pad_short_native ||
				packet->len == TBV_DATA_FRAME_SIZE ?
				0 : packet->len;
		f->frame.flags = 0;
		f->frame.sof = packet->sof;
		f->frame.eof = packet->eof;
		dma_sync_single_for_device(tb_ring_dma_device(path->tx_ring),
					   f->dma, TBV_DATA_FRAME_SIZE,
					   DMA_TO_DEVICE);

		/*
		 * Publish the frame as inflight before giving it to the ring.
		 * Once tb_ring_tx() accepts it, an interrupt completion may run
		 * on another CPU before tb_ring_tx() returns. Linking it only in
		 * the success branch lets that completion put f on tx_free and
		 * then makes this CPU link the same node into tx_frame_inflight,
		 * corrupting both lists.
		 */
		spin_lock_irqsave(&path->tx_lock, flags);
		f->posted_jiffies = jiffies;
		list_add_tail(&f->free_node, &path->tx_frame_inflight);
		spin_unlock_irqrestore(&path->tx_lock, flags);

		ret = tb_ring_tx(path->tx_ring, &f->frame);
		if (!ret) {
			if (state)
				atomic64_inc(&state->data_tx_posted);
			if (packet_control)
				atomic64_inc(&path->control_tx_posted);
			else
				atomic64_inc(&path->data_tx_posted);
			tbv_path_queue_tx_poll(path, 0);
			tbv_path_queue_rx_supp_poll(
				path,
				msecs_to_jiffies(TBV_RX_SUPP_POLL_DELAY_MS));
			continue;
		}

		if (state)
			atomic64_inc(&state->data_tx_errors);
		f->packet = NULL;
		f->done = NULL;
		f->done_ctx = NULL;
		f->frame.callback = NULL;
		spin_lock_irqsave(&path->tx_lock, flags);
		f->posted_jiffies = 0;
		list_del_init(&f->free_node);
		list_add_tail(&f->free_node, &path->tx_free);
		path->tx_raw_stream_active = old_raw_stream_active;
		path->tx_raw_stream_owner = old_raw_stream_owner;
		path->tx_raw_stream_end_seen = old_raw_stream_end_seen;
		path->tx_raw_stream_window_open = old_raw_stream_window_open;
		path->tx_raw_stream_inflight = old_raw_stream_inflight;
		packet->raw_stream_counted = false;
		if (charged_data_credit) {
			if (path->tx_remote_data_credits <
			    path->tx_remote_data_credit_max)
				path->tx_remote_data_credits++;
		}
		if (ret == -ENOMEM &&
		    path->state == TBV_PATH_TUNNEL_ENABLED &&
		    (packet->control || packet->done || packet->owner_ctx)) {
			if (packet->control) {
				list_add(&packet->node, &path->tx_control_queue);
				path->tx_control_queued++;
			} else {
				list_add(&packet->node, &path->tx_data_queue);
				path->tx_data_queued++;
				packet->start_credit_group_frames =
					old_start_credit_group_frames;
			}
			packet->queued = true;
			tbv_path_tx_scheduling_done_locked(path);
			spin_unlock_irqrestore(&path->tx_lock, flags);
			atomic_dec(&path->tx_inflight);
			return;
		}
		spin_unlock_irqrestore(&path->tx_lock, flags);
		atomic_dec(&path->tx_inflight);
		tbv_path_tx_packet_release(packet, ret);
		spin_lock_irqsave(&path->tx_lock, flags);
		tbv_path_tx_scheduling_done_locked(path);
		spin_unlock_irqrestore(&path->tx_lock, flags);
		return;
	}
}

bool tbv_path_tx_inflight_available(int inflight, u32 max_inflight)
{
	/*
	 * tx_inflight is shared by copied data, zero-copy data and control
	 * descriptors on one NHI ring. A transient negative value is a
	 * bookkeeping fault, not a reason to wedge the path forever.
	 */
	return inflight < 0 || (u32)inflight < max_inflight;
}

void tbv_path_kick_tx(struct tbv_path *path)
{
	if (path)
		tbv_path_schedule_tx(path);
}

int tbv_path_send(struct tbv_path *path, const void *data, u32 len,
		  unsigned int send_flags,
		  tbv_path_tx_done_fn done, void *done_ctx)
{
	struct tbv_tx_packet *packet;
	int ret;

	if (!len || len > TBV_DATA_FRAME_SIZE)
		return -EINVAL;
	if (send_flags & ~(TBV_PATH_SEND_CONTROL | TBV_PATH_SEND_DEFER))
		return -EINVAL;
	if (send_flags & TBV_PATH_SEND_CONTROL) {
		if (send_flags & TBV_PATH_SEND_DEFER)
			return -EINVAL;
		return tbv_path_enqueue_control(path, data, len, done, done_ctx);
	}

	packet = tbv_path_alloc_data_packet(path, data, len, done, done_ctx);
	if (!packet)
		return -ENOMEM;

	ret = tbv_path_enqueue_data(path, packet,
				    send_flags & TBV_PATH_SEND_DEFER);
	if (ret) {
		kfree(packet->buf);
		kfree(packet);
		return ret;
	}
	return 0;
}

int tbv_path_send_owned(struct tbv_path *path, void *data, u32 len,
			unsigned int send_flags,
			tbv_path_tx_done_fn done, void *done_ctx)
{
	return tbv_path_send_marked_owned(path, data, len,
					  TBV_DATA_PDF_FRAME_START,
					  TBV_DATA_PDF_FRAME_END,
					  send_flags, done, done_ctx);
}

int tbv_path_send_marked_owned(struct tbv_path *path, void *data, u32 len,
			       u8 sof, u8 eof, unsigned int send_flags,
			       tbv_path_tx_done_fn done, void *done_ctx)
{
	struct tbv_tx_packet *packet;
	int ret;

	if (!data)
		return -EINVAL;
	if (!len || len > TBV_DATA_FRAME_SIZE ||
	    (send_flags & ~(TBV_PATH_SEND_CONTROL | TBV_PATH_SEND_DEFER)) ||
	    (send_flags & TBV_PATH_SEND_CONTROL)) {
		kfree(data);
		return -EINVAL;
	}

	packet = tbv_path_alloc_data_packet_owned(path, data, len, done,
						  done_ctx);
	if (!packet) {
		kfree(data);
		return -ENOMEM;
	}
	packet->sof = sof;
	packet->eof = eof;

	ret = tbv_path_enqueue_data(path, packet,
				    send_flags & TBV_PATH_SEND_DEFER);
	if (ret) {
		kfree(packet->buf);
		kfree(packet);
		return ret;
	}
	return 0;
}

int tbv_path_send_marked_fill(struct tbv_path *path, u32 len,
			      u8 sof, u8 eof, unsigned int send_flags,
			      tbv_path_tx_fill_fn fill, void *fill_ctx,
			      tbv_path_tx_done_fn done, void *done_ctx)
{
	struct tbv_tx_packet *packet;
	int ret;

	if (!fill)
		return -EINVAL;
	if (!len || len > TBV_DATA_FRAME_SIZE ||
	    (send_flags & ~(TBV_PATH_SEND_CONTROL | TBV_PATH_SEND_DEFER)) ||
	    (send_flags & TBV_PATH_SEND_CONTROL))
		return -EINVAL;

	packet = tbv_path_alloc_pooled_data_packet(path, len, done, done_ctx);
	if (packet) {
		ret = fill(fill_ctx, packet->buf, len);
		if (ret) {
			packet->done = NULL;
			packet->done_ctx = NULL;
			tbv_path_tx_packet_release(packet, ret);
			return ret;
		}
	} else {
		packet = tbv_path_alloc_inline_data_packet(path, len, done,
							  done_ctx);
		if (!packet)
			return -ENOMEM;
		ret = fill(fill_ctx, packet->buf, len);
		if (ret) {
			packet->done = NULL;
			packet->done_ctx = NULL;
			tbv_path_tx_packet_release(packet, ret);
			return ret;
		}
	}

	packet->sof = sof;
	packet->eof = eof;
	ret = tbv_path_enqueue_data(path, packet,
				    send_flags & TBV_PATH_SEND_DEFER);
	if (ret) {
		packet->done = NULL;
		packet->done_ctx = NULL;
		tbv_path_tx_packet_release(packet, ret);
		return ret;
	}
	return 0;
}

static void tbv_path_release_packet_list(struct list_head *packets, int status)
{
	while (!list_empty(packets)) {
		struct tbv_tx_packet *packet =
			list_first_entry(packets, struct tbv_tx_packet, node);

		list_del_init(&packet->node);
		tbv_path_tx_packet_release(packet, status);
	}
}

static void tbv_path_release_packet_list_silent(struct list_head *packets,
						int status)
{
	while (!list_empty(packets)) {
		struct tbv_tx_packet *packet =
			list_first_entry(packets, struct tbv_tx_packet, node);

		list_del_init(&packet->node);
		packet->done = NULL;
		packet->done_ctx = NULL;
		tbv_path_tx_packet_release(packet, status);
	}
}

void tbv_path_release_prepared_list_silent(struct list_head *packets,
					   int status)
{
	tbv_path_release_packet_list_silent(packets, status);
}

struct tbv_path_cancel_done {
	tbv_path_tx_done_fn done;
	void *ctx;
};

static void tbv_path_cancel_record_done(struct tbv_path_cancel_done *done,
					u32 *done_count, u32 done_max,
					struct tbv_tx_packet *packet)
{
	if (!packet || !packet->done)
		return;
	if (*done_count >= done_max)
		return;

	done[*done_count].done = packet->done;
	done[*done_count].ctx = packet->done_ctx;
	(*done_count)++;
	packet->done = NULL;
	packet->done_ctx = NULL;
	packet->owner_ctx = NULL;
}

int tbv_path_prepare_filled(struct tbv_path *path,
			    struct list_head *packets, u32 len,
			    u8 sof, u8 eof, tbv_path_tx_fill_fn fill,
			    void *fill_ctx, tbv_path_tx_done_fn done,
			    void *done_ctx)
{
	struct tbv_tx_packet *packet;
	int ret;

	if (!path || !packets || !fill || !len ||
	    len > TBV_DATA_FRAME_SIZE)
		return -EINVAL;

	packet = tbv_path_alloc_inline_data_packet(path, len, done, done_ctx);
	if (!packet)
		return -ENOMEM;
	ret = fill(fill_ctx, packet->buf, len);
	if (ret) {
		packet->done = NULL;
		packet->done_ctx = NULL;
		tbv_path_tx_packet_release(packet, ret);
		return ret;
	}
	packet->sof = sof;
	packet->eof = eof;
	list_add_tail(&packet->node, packets);
	return 0;
}

int tbv_path_enqueue_prepared_reserved(struct tbv_path *path,
				       struct list_head *packets,
				       u32 packet_count,
				       unsigned int send_flags)
{
	int ret;

	if (!path || !packets)
		return -EINVAL;
	if (send_flags & ~(TBV_PATH_SEND_DEFER | TBV_PATH_SEND_PRIORITY)) {
		tbv_path_release_packet_list_silent(packets, -EINVAL);
		return -EINVAL;
	}

	ret = tbv_path_enqueue_reserved_data_list(
		path, packets, packet_count, send_flags & TBV_PATH_SEND_DEFER,
		send_flags & TBV_PATH_SEND_PRIORITY);
	if (ret)
		tbv_path_release_packet_list_silent(packets, ret);
	return ret;
}

int tbv_path_send_page_stream(struct tbv_path *path,
			      const struct tbv_native_data_header *hdr,
			      u32 total_length, unsigned int send_flags,
			      tbv_path_tx_done_fn meta_done,
			      void *meta_done_ctx,
			      tbv_path_next_page_fn next, void *next_ctx)
{
	struct device *dma_dev;
	LIST_HEAD(packets);
	u32 prepared = 0;
	u32 packet_count = 0;
	struct tbv_tx_packet *packet;
	u32 max_raw_payload;
	struct tbv_native_data_header stream_hdr;
	u8 *hdr_buf;
	int ret;

	if (!path || !hdr || !next || !total_length) {
		ret = -EINVAL;
		goto err_meta_done;
	}
	if (send_flags & ~(TBV_PATH_SEND_DEFER | TBV_PATH_SEND_REFUND |
			   TBV_PATH_SEND_PRIORITY)) {
		ret = -EINVAL;
		goto err_meta_done;
	}
	if (total_length > TBV_NATIVE_DATA_MAX_MSG_SIZE) {
		ret = -EMSGSIZE;
		goto err_meta_done;
	}
	if (!path->tx_ring) {
		ret = -ENOTCONN;
		goto err_meta_done;
	}
	if (hdr->opcode == TBV_NATIVE_DATA_OP_RDMA_WRITE ||
	    hdr->opcode == TBV_NATIVE_DATA_OP_RDMA_WRITE_IMM)
		max_raw_payload = TBV_DATA_FRAME_SIZE;
	else
		max_raw_payload = TBV_NATIVE_DATA_MAX_PAYLOAD;

	dma_dev = tb_ring_dma_device(path->tx_ring);
	if (!tbv_dma_device_ready(dma_dev)) {
		ret = -EPROBE_DEFER;
		goto err_meta_done;
	}

	hdr_buf = kzalloc(TBV_NATIVE_DATA_HDR_SIZE, GFP_KERNEL);
	if (!hdr_buf) {
		ret = -ENOMEM;
		goto err_release;
	}

	/*
	 * The caller owns F_LAST: a full-message stream sets it, a
	 * per-fragment split stream sets it only on the message's final
	 * fragment. Only the raw-stream marker is implied here.
	 */
	stream_hdr = *hdr;
	stream_hdr.length = total_length;
	stream_hdr.flags |= TBV_NATIVE_DATA_F_RAW_STREAM;
	ret = tbv_native_data_build_header(hdr_buf, TBV_NATIVE_DATA_HDR_SIZE,
					   &stream_hdr);
	if (ret < 0) {
		kfree(hdr_buf);
		goto err_release;
	}

	packet = tbv_path_alloc_data_packet_owned(path, hdr_buf,
						  TBV_NATIVE_DATA_HDR_SIZE,
						  meta_done, meta_done_ctx);
	if (!packet) {
		kfree(hdr_buf);
		ret = -ENOMEM;
		goto err_release;
	}
	packet->raw_stream_start = true;
	list_add_tail(&packet->node, &packets);
	packet_count++;

	while (prepared < total_length) {
		tbv_path_tx_done_fn done = NULL;
		struct page *page = NULL;
		void *done_ctx = NULL;
		void *owned = NULL;
		bool last;
		bool premapped = false;
		dma_addr_t dma = 0;
		u32 page_off = 0;
		u32 len = 0;
		u32 map_len = 0;

		ret = next(next_ctx, &page, &page_off, &len, &dma, &premapped,
			   &owned, &done, &done_ctx);
		if (ret) {
			kfree(owned);
			goto err_release;
		}
		if (!len || len > max_raw_payload ||
		    len > total_length - prepared ||
		    (!owned && !premapped &&
		     (!page || page_off > PAGE_SIZE ||
		      len > PAGE_SIZE - page_off))) {
			kfree(owned);
			if (done)
				done(done_ctx, -EINVAL);
			ret = -EINVAL;
			goto err_release;
		}

		last = prepared + len == total_length;

		if (owned) {
			/*
			 * zcopy_stage_payload: a kernel copy of the window.
			 * Frame it as a normal owned data packet so schedule_tx
			 * stages it into the ring's persistent kernel TX frame
			 * (memcpy + dma_sync), i.e. the NHI DMA-reads stable
			 * kernel memory. Isolates "reading live MR memory" from
			 * the split framing, which is otherwise unchanged.
			 */
			packet = tbv_path_alloc_data_packet_owned(path, owned,
								  len, done,
								  done_ctx);
			if (!packet) {
				kfree(owned);
				if (done)
					done(done_ctx, -ENOMEM);
				ret = -ENOMEM;
				goto err_release;
			}
			goto have_packet;
		}

		if (premapped) {
			/*
			 * DMA address from the MR's persistent umem mapping.
			 * No dma_map/unmap here -- this is the whole point: it
			 * eliminates the per-frame IOTLB churn that corrupted
			 * frames under lazy AMD-Vi invalidation. unmap_dma is
			 * false so completion never touches this address.
			 */
			packet = tbv_path_alloc_zcopy_packet(path, dma, len, 0,
							     false, done,
							     done_ctx);
			if (!packet) {
				if (done)
					done(done_ctx, -ENOMEM);
				ret = -ENOMEM;
				goto err_release;
			}
			goto have_packet;
		}

		/*
		 * Fallback (rail on a different DMA device than the MR was
		 * mapped to): map per frame. Map to the page boundary, not just
		 * the transmitted len, so an NHI read that rounds up past
		 * frame->size cannot fault the tail of the page (the copied
		 * path always maps a full 4096 buffer); frame->size below still
		 * transmits exactly len.
		 */
		map_len = tbv_zcopy_frame_map_len(page_off, len, PAGE_SIZE,
						  TBV_DATA_FRAME_SIZE,
						  READ_ONCE(zcopy_map_full_page));
		dma = dma_map_page(dma_dev, page, page_off, map_len,
				   DMA_TO_DEVICE);
		if (dma_mapping_error(dma_dev, dma)) {
			if (done)
				done(done_ctx, -EIO);
			ret = -EIO;
			goto err_release;
		}

		packet = tbv_path_alloc_zcopy_packet(path, dma, len, map_len,
						     true, done, done_ctx);
		if (!packet) {
			dma_unmap_page(dma_dev, dma, map_len, DMA_TO_DEVICE);
			if (done)
				done(done_ctx, -ENOMEM);
			ret = -ENOMEM;
			goto err_release;
		}
have_packet:
		packet->owner_ctx = meta_done_ctx ? meta_done_ctx : done_ctx;
		packet->raw_stream_end = last;
		list_add_tail(&packet->node, &packets);
		packet_count++;
		prepared += len;
	}

	/*
	 * Retransmit: reclaim the previous attempt's credits for exactly the
	 * frames this attempt re-charges (a lost frame's credit is never
	 * returned by the peer; see tbv_path_refund_remote_data_credits).
	 */
	if (send_flags & TBV_PATH_SEND_REFUND)
		tbv_path_refund_remote_data_credits(path, packet_count);

	ret = tbv_path_enqueue_data_list(path, &packets, packet_count,
					 send_flags & TBV_PATH_SEND_DEFER,
					 send_flags & TBV_PATH_SEND_PRIORITY);
	if (ret)
		goto err_release;
	return 0;

err_release:
	if (!packet_count && meta_done)
		meta_done(meta_done_ctx, ret);
	tbv_path_release_packet_list(&packets, ret);
	return ret;

err_meta_done:
	if (meta_done)
		meta_done(meta_done_ctx, ret);
	return ret;
}

static bool tbv_path_packet_matches(const struct tbv_tx_packet *packet,
				    tbv_path_tx_done_fn done, void *done_ctx,
				    void *owner_ctx)
{
	if (owner_ctx && packet->owner_ctx == owner_ctx)
		return true;
	return done && done_ctx && packet->done == done &&
	       packet->done_ctx == done_ctx;
}

static void tbv_path_cancel_data_match(struct tbv_path *path,
				       tbv_path_tx_done_fn done, void *done_ctx,
				       void *owner_ctx)
{
	struct tbv_tx_packet *packet;
	struct tbv_tx_packet *tmp;
	struct tbv_path_cancel_done *done_list;
	LIST_HEAD(cancel);
	unsigned long flags;
	void *stream_owner;
	u32 i;
	u32 done_count = 0;
	u32 done_max;
	bool raw_stream_canceled = false;

	if ((!done || !done_ctx) && !owner_ctx)
		return;

	done_max = max_t(u32, path->tx_frame_count, 1);
	done_list = kvcalloc(done_max, sizeof(*done_list), GFP_KERNEL);
	if (!done_list)
		return;

	spin_lock_irqsave(&path->tx_lock, flags);
	stream_owner = path->tx_raw_stream_active ? path->tx_raw_stream_owner :
						    NULL;
	if (owner_ctx && stream_owner == owner_ctx)
		raw_stream_canceled = true;
	list_for_each_entry_safe(packet, tmp, &path->tx_data_queue, node) {
		if (!tbv_path_packet_matches(packet, done, done_ctx,
					     owner_ctx))
			continue;
		/*
		 * Cancelling by (done, done_ctx) takes packets out of an open
		 * unframed window without ever naming its owner, and the
		 * window closes only when its own end packet is dequeued. Lose
		 * that packet and tbv_path_schedule_tx() blocks on the gate
		 * forever: the head of the queue belongs to somebody else, so
		 * nothing is posted, so no completion ever closes the window.
		 * A cancelled member of the open window closes it.
		 */
		if (stream_owner && packet->owner_ctx == stream_owner)
			raw_stream_canceled = true;
		list_del_init(&packet->node);
		packet->queued = false;
			if (path->tx_data_queued)
				path->tx_data_queued--;
			packet->owner_ctx = NULL;
			list_add_tail(&packet->node, &cancel);
	}
	if (raw_stream_canceled) {
		path->tx_raw_stream_active = false;
		path->tx_raw_stream_owner = NULL;
		path->tx_raw_stream_end_seen = false;
		path->tx_raw_stream_window_open = false;
		path->tx_raw_stream_inflight = 0;
	}

	for (i = 0; i < path->tx_frame_count; i++) {
		struct tbv_data_frame *f = &path->tx_frames[i];

		packet = f->packet;
		if (!packet || packet->control ||
		    !tbv_path_packet_matches(packet, done, done_ctx,
					     owner_ctx))
			continue;
		tbv_path_cancel_record_done(done_list, &done_count, done_max,
					    packet);
		f->done = NULL;
		f->done_ctx = NULL;
	}

	list_for_each_entry_safe(packet, tmp, &path->tx_zcopy_inflight, node) {
		if (!tbv_path_packet_matches(packet, done, done_ctx,
					     owner_ctx))
			continue;
		list_del_init(&packet->node);
		packet->inflight = false;
		tbv_path_cancel_record_done(done_list, &done_count, done_max,
					    packet);
	}
	spin_unlock_irqrestore(&path->tx_lock, flags);

	while (!list_empty(&cancel)) {
		packet = list_first_entry(&cancel, struct tbv_tx_packet, node);
		list_del_init(&packet->node);
		tbv_path_tx_packet_release(packet, -ECANCELED);
	}

	for (i = 0; i < done_count; i++)
		done_list[i].done(done_list[i].ctx, -ECANCELED);

	kvfree(done_list);

	if (raw_stream_canceled)
		tbv_path_schedule_tx(path);
}

void tbv_path_cancel_data_done_ctx(struct tbv_path *path,
				   tbv_path_tx_done_fn done, void *done_ctx)
{
	tbv_path_cancel_data_match(path, done, done_ctx, NULL);
}

void tbv_path_cancel_data_owner_ctx(struct tbv_path *path, void *owner_ctx)
{
	tbv_path_cancel_data_match(path, NULL, NULL, owner_ctx);
}

static void tbv_path_flush_tx_queue(struct tbv_path *path, int status)
{
	LIST_HEAD(control);
	LIST_HEAD(data);
	unsigned long flags;

	spin_lock_irqsave(&path->tx_lock, flags);
	list_splice_init(&path->tx_control_queue, &control);
	list_splice_init(&path->tx_data_queue, &data);
	path->tx_control_queued = 0;
	path->tx_data_queued = 0;
	path->tx_data_reserved = 0;
	tbv_path_tx_scheduling_done_locked(path);
	path->tx_raw_stream_active = false;
	path->tx_raw_stream_owner = NULL;
	path->tx_raw_stream_end_seen = false;
	path->tx_raw_stream_window_open = false;
	path->tx_raw_stream_inflight = 0;
	spin_unlock_irqrestore(&path->tx_lock, flags);

	while (!list_empty(&control)) {
		struct tbv_tx_packet *packet =
			list_first_entry(&control, struct tbv_tx_packet, node);

		list_del_init(&packet->node);
		tbv_path_tx_packet_release(packet, status);
	}

	while (!list_empty(&data)) {
		struct tbv_tx_packet *packet =
			list_first_entry(&data, struct tbv_tx_packet, node);

		list_del_init(&packet->node);
		tbv_path_tx_packet_release(packet, status);
	}
}

typedef int (*tbv_path_reload_step_fn)(void *ctx,
				       enum tbv_test_reload_step step);

/* Exact ordering shared by production rail removal and the KUnit trace. */
static int tbv_path_run_reload_sequence(tbv_path_reload_step_fn run, void *ctx)
{
	static const enum tbv_test_reload_step order[] = {
		TBV_TEST_RELOAD_DISABLE_ADMISSION,
		TBV_TEST_RELOAD_DRAIN_TX_SCHEDULER,
		TBV_TEST_RELOAD_DISABLE_ICM_PATH,
		TBV_TEST_RELOAD_STOP_RX_RING,
		TBV_TEST_RELOAD_STOP_TX_RING,
	};
	int first_error = 0;
	u32 i;

	for (i = 0; i < ARRAY_SIZE(order); i++) {
		int ret = run(ctx, order[i]);

		/* Teardown must keep fencing even if the ICM disconnect failed. */
		if (ret && !first_error)
			first_error = ret;
	}
	return first_error;
}

struct tbv_path_reload_ctx {
	struct tbv_path *path;
	struct tb_xdomain *xd;
	bool tunnel_enabled;
	bool rings_started;
};

static int tbv_path_run_reload_step(void *data,
				    enum tbv_test_reload_step step)
{
	struct tbv_path_reload_ctx *ctx = data;
	struct tbv_path *path = ctx->path;
	unsigned long flags;
	int ret = 0;

	switch (step) {
	case TBV_TEST_RELOAD_DISABLE_ADMISSION:
		/* All enqueue/reserve paths gate on state under tx_lock. */
		spin_lock_irqsave(&path->tx_lock, flags);
		if (path->state == TBV_PATH_TUNNEL_ENABLED)
			path->state = TBV_PATH_RING_STARTED;
		spin_unlock_irqrestore(&path->tx_lock, flags);
		break;
	case TBV_TEST_RELOAD_DRAIN_TX_SCHEDULER:
		/*
		 * A scheduler can be between its state check and tb_ring_tx().
		 * Admission is closed above, so wait until that last publisher
		 * returns before disconnecting or stopping the hardware path.
		 */
		wait_event(path->tx_idle_wait,
			   !READ_ONCE(path->tx_scheduling));
		break;
	case TBV_TEST_RELOAD_DISABLE_ICM_PATH:
		if (!ctx->tunnel_enabled)
			break;
		if (destroy_disable_paths)
			ret = tb_xdomain_disable_paths(ctx->xd,
						 path->local_transmit_path,
						 path->local_tx_hop,
						 path->remote_transmit_path,
						 path->local_rx_hop);
		else
			pr_warn("skipping tb_xdomain_disable_paths during rail quiesce route=0x%llx rail=0x%x\n",
				path->rail && path->rail->peer ?
					path->rail->peer->xd->route : 0,
				path->rail ? path->rail->rail_id : 0xffffffff);
		if (path->remote_transmit_path >= 0) {
			tb_xdomain_release_in_hopid(ctx->xd,
						 path->remote_transmit_path);
			path->remote_transmit_path = -1;
		}
		break;
	case TBV_TEST_RELOAD_STOP_RX_RING:
		if (ctx->rings_started && !path->rings_fenced && path->rx_ring)
			tb_ring_stop(path->rx_ring);
		break;
	case TBV_TEST_RELOAD_STOP_TX_RING:
		if (ctx->rings_started && !path->rings_fenced && path->tx_ring)
			tb_ring_stop(path->tx_ring);
		break;
	default:
		ret = -EINVAL;
		break;
	}
	return ret;
}

int tbv_path_quiesce(struct tbv_path *path, struct tb_xdomain *xd)
{
	struct tbv_path_reload_ctx ctx = {
		.path = path,
		.xd = xd,
		.tunnel_enabled = path->state == TBV_PATH_TUNNEL_ENABLED,
		.rings_started = path->state == TBV_PATH_TUNNEL_ENABLED ||
				 path->state == TBV_PATH_RING_STARTED,
	};
	int ret;

	might_sleep();
	ret = tbv_path_run_reload_sequence(tbv_path_run_reload_step, &ctx);
	if (ctx.rings_started) {
		path->rings_fenced = true;
		path->state = TBV_PATH_RING_ALLOCATED;
	}
	cancel_delayed_work_sync(&path->tx_poll_work);
	cancel_delayed_work_sync(&path->rx_supp_poll_work);
	cancel_delayed_work_sync(&path->credit_sync_work);
	return ret;
}

#if IS_ENABLED(CONFIG_KUNIT)
struct tbv_test_path_reload_ctx {
	u8 *steps;
	u32 capacity;
	u32 count;
};

static int tbv_test_path_record_reload_step(void *data,
					    enum tbv_test_reload_step step)
{
	struct tbv_test_path_reload_ctx *ctx = data;

	if (!ctx->steps || ctx->count >= ctx->capacity)
		return -ENOSPC;
	ctx->steps[ctx->count++] = step;
	return 0;
}

int tbv_test_path_reload_order(u8 *steps, u32 capacity, u32 *count)
{
	struct tbv_test_path_reload_ctx ctx = {
		.steps = steps,
		.capacity = capacity,
	};
	int ret;

	if (!count)
		return -EINVAL;
	ret = tbv_path_run_reload_sequence(tbv_test_path_record_reload_step,
					   &ctx);
	*count = ctx.count;
	return ret;
}

bool tbv_test_path_scheduler_wake(enum tbv_path_state state)
{
	return tbv_path_scheduler_wake_needed(state);
}
#endif

#if IS_ENABLED(CONFIG_KUNIT)
struct tbv_test_queue_timeout_ctx {
	u32 done_calls;
	int last_status;
};

static void tbv_test_queue_timeout_done(void *ctx, int status)
{
	struct tbv_test_queue_timeout_ctx *c = ctx;

	c->done_calls++;
	c->last_status = status;
}

/*
 * Scenario hook for tests/credit_stall_recovery_test.c. Builds a real
 * tbv_path whose peer has stopped returning data credits, enqueues real data
 * packets through the real tbv_path_send() path, ages them, and runs the real
 * TX work.
 *
 * Field provenance (only states the driver can actually produce):
 *  - tx_remote_data_credits = 0 with a nonzero max is what
 *    tbv_path_schedule_tx() leaves behind after charging the window down while
 *    no PATH_CREDIT frame comes back; only tbv_path_add_remote_rx_credits(),
 *    tbv_path_refund_remote_data_credits() and
 *    tbv_path_set_remote_rx_capacity() raise it again, and all three need the
 *    peer.
 *  - one frame on tx_free with tx_frame_count = 1 mirrors
 *    tbv_path_alloc_frames(): the packet must clear the staging-frame checks so
 *    the CREDIT gate is what stops it, which data_tx_credit_stalls confirms.
 *  - packet->queued_jiffies is stamped by the enqueue paths and cleared only by
 *    tbv_path_tx_packet_release(), so back-dating it is exactly "this packet
 *    has been queued that long".
 *  - tx_ring stays NULL: no frame ever reaches a ring in a credit stall, and
 *    the gate returns before any tb_ring_tx().
 */
/*
 * Scenario hook for tests/credit_stall_recovery_test.c.
 *
 * Field provenance:
 *  - tbv_path_set_remote_rx_capacity() is the real HELLO-time seeding, so the
 *    window starts full and the resync baseline starts unprimed exactly as on
 *    a live path.
 *  - tx_remote_data_credits = 0 is what tbv_path_schedule_tx() leaves after
 *    charging one credit per frame for a full window.
 *  - the surviving PATH_CREDIT delta goes through the real
 *    tbv_path_add_remote_rx_credits(), which is the only thing a peer's credit
 *    frame does, and the resync through the real
 *    tbv_path_sync_remote_rx_credits().
 *  - no rail, no ring: neither credit path touches either.
 */
int tbv_test_path_credit_resync(u32 total, u32 lost, bool deliver_sync,
				u32 *credits_out, u32 *max_out)
{
	struct tbv_path_config cfg;
	struct tbv_path *path;

	path = kzalloc(sizeof(*path), GFP_KERNEL);
	if (!path)
		return -ENOMEM;

	tbv_path_default_config(TBV_BACKEND_NATIVE, &cfg);
	tbv_path_init(path, &cfg, NULL);
	tbv_path_set_remote_rx_capacity(path, cfg.rx_ring_size);
	if (total > path->tx_remote_data_credit_max ||
	    lost > total) {
		kfree(path);
		return -EINVAL;
	}

	/* Steady state: the peer has resynced its baseline at least once. */
	tbv_path_sync_remote_rx_credits(path, 0);

	/* The sender charged the whole window down and is now gated. */
	path->tx_remote_data_credits = 0;

	/* The peer returned @total but the frame carrying @lost was dropped. */
	if (total - lost)
		tbv_path_add_remote_rx_credits(path, total - lost);
	if (deliver_sync)
		tbv_path_sync_remote_rx_credits(path, total);

	if (credits_out)
		*credits_out = path->tx_remote_data_credits;
	if (max_out)
		*max_out = path->tx_remote_data_credit_max;
	kfree(path);
	return 0;
}

int tbv_test_path_credit_stale_sync(u32 *credits_out, u32 *seen_out,
				    u64 *recovered_out)
{
	struct tbv_path_config cfg;
	struct tbv_path *path;

	path = kzalloc(sizeof(*path), GFP_KERNEL);
	if (!path)
		return -ENOMEM;

	tbv_path_default_config(TBV_BACKEND_NATIVE, &cfg);
	tbv_path_init(path, &cfg, NULL);
	tbv_path_set_remote_rx_capacity(path, cfg.rx_ring_size);
	tbv_path_sync_remote_rx_credits(path, 0);
	tbv_path_add_remote_rx_credits(path, 100);

	/* Model a fully charged-down sender, then deliver an older sync. */
	path->tx_remote_data_credits = 0;
	tbv_path_sync_remote_rx_credits(path, 99);

	if (credits_out)
		*credits_out = path->tx_remote_data_credits;
	if (seen_out)
		*seen_out = path->tx_remote_credit_returned_seen;
	if (recovered_out)
		*recovered_out =
			atomic64_read(&path->data_tx_credit_resync_recovered);
	kfree(path);
	return 0;
}

int tbv_test_path_credit_mode(bool local_e2e, bool remote_e2e,
			      u32 remote_caps, u32 *credits_out, u32 *max_out)
{
	struct tbv_peer peer = {
		.backend = TBV_BACKEND_NATIVE,
		.remote_caps = remote_caps,
	};
	struct tbv_rail rail = {
		.peer = &peer,
	};
	struct tbv_path_config cfg;
	struct tbv_path *path;

	path = kzalloc(sizeof(*path), GFP_KERNEL);
	if (!path)
		return -ENOMEM;

	tbv_path_default_config(TBV_BACKEND_NATIVE, &cfg);
	cfg.e2e = local_e2e;
	tbv_path_init(path, &cfg, &rail);
	path->remote_e2e = remote_e2e;
	tbv_path_set_remote_rx_capacity(path, cfg.rx_ring_size);

	if (credits_out)
		*credits_out = path->tx_remote_data_credits;
	if (max_out)
		*max_out = path->tx_remote_data_credit_max;
	kfree(path);
	return 0;
}

struct tbv_test_inline_fill_ctx {
	u32 calls;
	u8 value;
};

static int tbv_test_inline_fill(void *ctx, void *dst, u32 len)
{
	struct tbv_test_inline_fill_ctx *fill = ctx;

	fill->calls++;
	memset(dst, fill->value, len);
	return 0;
}

int tbv_test_path_inline_prepare(u32 len, u32 *fill_calls_out,
				 bool *inline_out, bool *data_match_out)
{
	struct tbv_test_inline_fill_ctx fill = {
		.value = 0xa5,
	};
	struct tbv_tx_packet *packet;
	struct tbv_path path = {};
	LIST_HEAD(packets);
	bool matches = true;
	u32 i;
	int ret;

	ret = tbv_path_prepare_filled(
		&path, &packets, len, TBV_DATA_PDF_FRAME_START,
		TBV_DATA_PDF_FRAME_END, tbv_test_inline_fill, &fill, NULL,
		NULL);
	if (ret)
		return ret;

	packet = list_first_entry(&packets, struct tbv_tx_packet, node);
	for (i = 0; i < len; i++) {
		if (packet->buf[i] != fill.value) {
			matches = false;
			break;
		}
	}
	if (fill_calls_out)
		*fill_calls_out = fill.calls;
	if (inline_out)
		*inline_out = packet->inline_buf &&
			      packet->buf == (u8 *)(packet + 1);
	if (data_match_out)
		*data_match_out = matches;

	tbv_path_release_packet_list_silent(&packets, -ECANCELED);
	return 0;
}

int tbv_test_path_credit_stall_timeout(u32 age_ms, u32 timeout_ms,
				       u32 *queued_out, u32 *stalls_out,
				       u32 *done_calls_out, int *status_out)
{
	struct tbv_test_queue_timeout_ctx ctx = {};
	struct tbv_data_frame *frame = NULL;
	struct tbv_path_config cfg;
	struct tbv_tx_packet *packet;
	struct tbv_path *path = NULL;
	uint saved_timeout_ms;
	u8 payload[64] = {};
	unsigned long flags;
	int ret;

	path = kzalloc(sizeof(*path), GFP_KERNEL);
	frame = kzalloc(sizeof(*frame), GFP_KERNEL);
	if (!path || !frame) {
		ret = -ENOMEM;
		goto out;
	}

	tbv_path_default_config(TBV_BACKEND_NATIVE, &cfg);
	tbv_path_init(path, &cfg, NULL);
	path->state = TBV_PATH_TUNNEL_ENABLED;
	path->tx_data_queue_limit = 8;
	path->tx_remote_data_credit_max = 4;
	path->tx_remote_data_credits = 0;
	frame->path = path;
	frame->tx = true;
	INIT_LIST_HEAD(&frame->frame.list);
	INIT_LIST_HEAD(&frame->free_node);
	list_add_tail(&frame->free_node, &path->tx_free);
	path->tx_frames = frame;
	path->tx_frame_count = 1;

	ret = tbv_path_send(path, payload, sizeof(payload), 0,
			    tbv_test_queue_timeout_done, &ctx);
	if (ret)
		goto out;

	spin_lock_irqsave(&path->tx_lock, flags);
	list_for_each_entry(packet, &path->tx_data_queue, node)
		packet->queued_jiffies = jiffies - msecs_to_jiffies(age_ms);
	spin_unlock_irqrestore(&path->tx_lock, flags);

	saved_timeout_ms = tx_queue_timeout_ms;
	tx_queue_timeout_ms = timeout_ms;
	tbv_path_tx_poll_work(&path->tx_poll_work.work);
	tx_queue_timeout_ms = saved_timeout_ms;

	if (queued_out)
		*queued_out = path->tx_data_queued;
	if (stalls_out)
		*stalls_out = atomic64_read(&path->data_tx_credit_stalls);
	if (done_calls_out)
		*done_calls_out = ctx.done_calls;
	if (status_out)
		*status_out = ctx.last_status;
	ret = 0;

out:
	if (path) {
		path->state = TBV_PATH_STOPPED;
		tbv_path_flush_tx_queue(path, -ECANCELED);
	}
	kfree(frame);
	kfree(path);
	return ret;
}

/*
 * Scenario hook for tests/tx_ring_stall_test.c -- replay of a captured field
 * hang, not a hand-invented state. See
 * drivers/thunderbolt_ibverbs/traces/2026-07-28-tx-ring-stall/ for the capture
 * this is built from.
 *
 * Field provenance (every field below is a state the driver itself produces on
 * the wire, taken from the appmana-019/appmana-008 capture):
 *  - tx_ring non-NULL, running, with an outstanding uncompleted descriptor is
 *    "frames were posted and the NHI never completed them". The observed rail
 *    read `tx_poll enabled=1 calls=282365 completed=0` with
 *    `data_tx_posted=2 data_tx_completed=0`, so the poll ran a quarter of a
 *    million times against a ring that returned nothing. tb_ring_poll() is the
 *    real core function; a running ring whose tail descriptor lacks
 *    RING_DESC_COMPLETED is exactly what it sees on that hardware.
 *  - tx_inflight = 1 is what tbv_path_schedule_tx() leaves after handing a
 *    frame to tb_ring_tx(); only a ring completion or an error unwind lowers it.
 *  - the packet is off tx_data_queue with queued = false because
 *    tbv_path_schedule_tx() dequeues before posting. A COPIED packet (the
 *    fleet's mode -- every capture shows data_wr_zcopy = 0 and
 *    data_wr_copied > 0) is then referenced only by the ring frame: it is on
 *    neither tx_data_queue nor tx_zcopy_inflight, so neither the queue ceiling
 *    nor tbv_path_flush_tx_queue() can reach it.
 *  - tx_last_progress_jiffies back-dated by @stall_ms is "no completion for
 *    that long", which is the only clock tbv_path_tx_stalled() and the poll's
 *    own stall warning consult.
 *
 * Runs the real tbv_path_tx_poll_work() @passes times and reports whether the
 * packet's owner was ever completed. Its done callback is the only thing that
 * drains the owning WR's tx_pending, so a run where done_calls stays 0 is the
 * hang: the poll re-arms forever and the WR can never finish.
 */
int tbv_test_path_tx_ring_stall(u32 passes, u32 stall_ms, u32 timeout_ms,
				u32 *passes_used_out, u32 *inflight_out,
				u32 *done_calls_out, int *status_out,
				u64 *poll_calls_out, u64 *poll_completed_out,
				bool *tx_stalled_out)
{
	struct tbv_test_queue_timeout_ctx ctx = {};
	struct tbv_data_frame *frame = NULL;
	struct tbv_tx_packet *packet = NULL;
	struct tb_ring *ring = NULL;
	struct tbv_path_config cfg;
	struct tbv_path *path = NULL;
	uint saved_timeout_ms;
	uint saved_poll_ms;
	u8 payload[64] = {};
	unsigned long flags;
	u32 pass = 0;
	int ret;

	path = kzalloc(sizeof(*path), GFP_KERNEL);
	frame = kzalloc(sizeof(*frame), GFP_KERNEL);
	ring = kzalloc(sizeof(*ring), GFP_KERNEL);
	if (!path || !frame || !ring) {
		ret = -ENOMEM;
		goto out;
	}

	tbv_path_default_config(TBV_BACKEND_NATIVE, &cfg);
	tbv_path_init(path, &cfg, NULL);
	path->state = TBV_PATH_TUNNEL_ENABLED;
	path->tx_data_queue_limit = 8;

	/*
	 * The credit gate holds the packet at enqueue only so that this hook,
	 * not real hardware, decides when it becomes "posted": tbv_path_send()
	 * would otherwise run all the way into tb_ring_tx(). The posted state
	 * is then built by hand below, exactly as tbv_path_schedule_tx() leaves
	 * it. Credits are irrelevant to what is under test -- the captured node
	 * had a healthy window (tx_credits at max) and a dead ring.
	 */
	path->tx_remote_data_credit_max = 4;
	path->tx_remote_data_credits = 0;

	frame->path = path;
	frame->tx = true;
	INIT_LIST_HEAD(&frame->frame.list);
	INIT_LIST_HEAD(&frame->free_node);
	list_add_tail(&frame->free_node, &path->tx_free);
	path->tx_frames = frame;
	path->tx_frame_count = 1;

	/*
	 * A running TX ring that yields no completion. tb_ring_poll() is the
	 * real core function; head == tail makes it return NULL on every pass
	 * without touching the core-private descriptor ring, which is the
	 * captured calls>0 / completed=0 exactly as the driver sees it -- the
	 * driver cannot tell an empty ring from one whose descriptors the
	 * hardware never completes, and neither can this test.
	 */
	spin_lock_init(&ring->lock);
	INIT_LIST_HEAD(&ring->queue);
	INIT_LIST_HEAD(&ring->in_flight);
	ring->is_tx = true;
	ring->size = 4;
	ring->head = 0;
	ring->tail = 0;
	ring->running = true;
	path->tx_ring = ring;
	path->tx_poll_enabled = true;

	ret = tbv_path_send(path, payload, sizeof(payload), 0,
			    tbv_test_queue_timeout_done, &ctx);
	if (ret)
		goto out;

	/*
	 * Take the packet the way tbv_path_schedule_tx() does just before
	 * tb_ring_tx(): off the queue, no longer queued, counted inflight. From
	 * here only a ring completion refers to it.
	 */
	spin_lock_irqsave(&path->tx_lock, flags);
	packet = list_first_entry_or_null(&path->tx_data_queue,
					  struct tbv_tx_packet, node);
	if (packet) {
		list_del_init(&packet->node);
		packet->queued = false;
		packet->queued_jiffies = 0;
		if (path->tx_data_queued)
			path->tx_data_queued--;
	}
	spin_unlock_irqrestore(&path->tx_lock, flags);
	if (!packet) {
		ret = -ENODATA;
		goto out;
	}
	atomic_inc(&path->tx_inflight);
	atomic64_inc(&path->data_tx_posted);
	/*
	 * A posted copied frame is tracked on tx_frame_inflight with the time
	 * it was handed to the ring; that is the only handle on it once it
	 * leaves the queue, so the posted state is not faithful without it.
	 */
	spin_lock_irqsave(&path->tx_lock, flags);
	frame->packet = packet;
	frame->done = packet->done;
	frame->done_ctx = packet->done_ctx;
	frame->posted_jiffies = jiffies;
	list_del_init(&frame->free_node);
	list_add_tail(&frame->free_node, &path->tx_frame_inflight);
	spin_unlock_irqrestore(&path->tx_lock, flags);

	saved_timeout_ms = tx_queue_timeout_ms;
	saved_poll_ms = tx_poll_delay_ms;
	tx_queue_timeout_ms = timeout_ms;
	/* 0 would make the poll a no-op re-arm; keep the real cadence. */
	tx_poll_delay_ms = tx_poll_delay_ms ?: 1;

	for (pass = 1; pass <= passes; pass++) {
		/*
		 * Each pass is one poll interval with no completion, so the
		 * age of the last progress grows by @stall_ms. The poll work
		 * itself may reset this stamp; that reset is part of what is
		 * under test.
		 */
		WRITE_ONCE(path->tx_last_progress_jiffies,
			   READ_ONCE(path->tx_last_progress_jiffies) -
				   msecs_to_jiffies(stall_ms));
		/* The frame ages by the same interval; the ring is dead. */
		spin_lock_irqsave(&path->tx_lock, flags);
		if (!list_empty(&path->tx_frame_inflight))
			frame->posted_jiffies -= msecs_to_jiffies(stall_ms);
		spin_unlock_irqrestore(&path->tx_lock, flags);
		tbv_path_tx_poll_work(&path->tx_poll_work.work);
		if (ctx.done_calls)
			break;
	}

	tx_queue_timeout_ms = saved_timeout_ms;
	tx_poll_delay_ms = saved_poll_ms;

	if (passes_used_out)
		*passes_used_out = min(pass, passes);
	if (inflight_out)
		*inflight_out = (u32)max(atomic_read(&path->tx_inflight), 0);
	if (done_calls_out)
		*done_calls_out = ctx.done_calls;
	if (status_out)
		*status_out = ctx.last_status;
	if (poll_calls_out)
		*poll_calls_out = atomic64_read(&path->tx_poll_calls);
	if (poll_completed_out)
		*poll_completed_out = atomic64_read(&path->tx_poll_completed);
	if (tx_stalled_out)
		*tx_stalled_out = tbv_path_tx_stalled(path);
	ret = 0;

out:
	if (path) {
		path->state = TBV_PATH_STOPPED;
		path->tx_ring = NULL;
		path->tx_poll_enabled = false;
		cancel_delayed_work_sync(&path->tx_poll_work);
		tbv_path_flush_tx_queue(path, -ECANCELED);
		/*
		 * The leak the test is about: a posted copied packet is on no
		 * list, so the flush above cannot see it. Free it here so the
		 * KUnit run itself stays clean.
		 */
		if (packet && !ctx.done_calls)
			tbv_path_tx_packet_release(packet, -ECANCELED);
	}
	kfree(ring);
	kfree(frame);
	kfree(path);
	return ret;
}

/*
 * Shared setup for the two interrupt-completion hooks below: a tunnel-enabled
 * native path with @frames staging frames, a running TX ring, and @frames
 * copied packets taken to exactly the state tbv_path_schedule_tx() leaves
 * behind after tb_ring_tx() -- off tx_data_queue, on tx_frame_inflight,
 * counted in tx_inflight, referenced only by their ring frame.
 *
 * The credit window is held closed so the enqueue stops at the queue and this
 * hook, not real hardware, decides when a packet becomes posted; the captured
 * nodes all had a healthy window (tx_credits at max) alongside the failure.
 */
/*
 * tb_ring_dma_device() dereferences ring->nhi->pdev->dev and the completion
 * path dma_syncs against it, so a ring under test needs that chain to exist.
 * The frames carry no mapping (dma 0), which on a zeroed device is a no-op.
 */
struct tbv_test_ring_dma {
	struct tb_nhi nhi;
	struct pci_dev pdev;
};

static int tbv_test_path_post_frames(struct tbv_path *path,
				     struct tb_ring *ring,
				     struct tbv_test_ring_dma *dma,
				     struct tbv_data_frame *frames, u32 count,
				     struct tbv_tx_packet **packets,
				     struct tbv_test_queue_timeout_ctx *ctx)
{
	struct tbv_path_config cfg;
	u8 payload[64] = {};
	unsigned long flags;
	u32 i;
	int ret;

	tbv_path_default_config(TBV_BACKEND_NATIVE, &cfg);
	tbv_path_init(path, &cfg, NULL);
	path->state = TBV_PATH_TUNNEL_ENABLED;
	path->tx_data_queue_limit = 8;
	path->tx_remote_data_credit_max = 4;
	path->tx_remote_data_credits = 0;

	spin_lock_init(&ring->lock);
	INIT_LIST_HEAD(&ring->queue);
	INIT_LIST_HEAD(&ring->in_flight);
	ring->is_tx = true;
	ring->size = 4;
	ring->running = true;
	dma->nhi.pdev = &dma->pdev;
	ring->nhi = &dma->nhi;
	path->tx_ring = ring;
	path->tx_poll_enabled = true;

	path->tx_frames = frames;
	path->tx_frame_count = count;
	for (i = 0; i < count; i++) {
		frames[i].path = path;
		frames[i].tx = true;
		INIT_LIST_HEAD(&frames[i].frame.list);
		INIT_LIST_HEAD(&frames[i].free_node);
		list_add_tail(&frames[i].free_node, &path->tx_free);
	}

	for (i = 0; i < count; i++) {
		struct tbv_tx_packet *packet;

		ret = tbv_path_send(path, payload, sizeof(payload), 0,
				    tbv_test_queue_timeout_done, ctx);
		if (ret)
			return ret;

		spin_lock_irqsave(&path->tx_lock, flags);
		packet = list_first_entry_or_null(&path->tx_data_queue,
						  struct tbv_tx_packet, node);
		if (packet) {
			list_del_init(&packet->node);
			packet->queued = false;
			packet->queued_jiffies = 0;
			if (path->tx_data_queued)
				path->tx_data_queued--;
		}
		spin_unlock_irqrestore(&path->tx_lock, flags);
		if (!packet)
			return -ENODATA;

		atomic_inc(&path->tx_inflight);
		atomic64_inc(&path->data_tx_posted);
		spin_lock_irqsave(&path->tx_lock, flags);
		frames[i].packet = packet;
		frames[i].done = packet->done;
		frames[i].done_ctx = packet->done_ctx;
		/* The callback the ring will invoke, as the post installs it. */
		frames[i].frame.callback = tbv_path_tx_complete;
		frames[i].posted_jiffies = jiffies;
		list_del_init(&frames[i].free_node);
		list_add_tail(&frames[i].free_node, &path->tx_frame_inflight);
		spin_unlock_irqrestore(&path->tx_lock, flags);
		packets[i] = packet;
	}
	return 0;
}

static void tbv_test_path_teardown(struct tbv_path *path,
				   struct tbv_tx_packet **packets, u32 count)
{
	u32 i;

	path->state = TBV_PATH_STOPPED;
	path->tx_ring = NULL;
	path->tx_poll_enabled = false;
	cancel_delayed_work_sync(&path->tx_poll_work);
	tbv_path_flush_tx_queue(path, -ECANCELED);
	for (i = 0; i < count; i++)
		if (packets[i])
			tbv_path_tx_packet_release(packets[i], -ECANCELED);
}

/*
 * Scenario hook for tests/tx_ring_progress_test.c -- the field's "tx stall
 * route=... inflight=1 posted=N completed=N-1" line, reproduced.
 *
 * Field provenance, from traces/baseline-025-023/hang-appmana-023.txt: that
 * warning repeats every 5.1 s on two healthy paths at once, and between two
 * consecutive lines BOTH posted and completed advance by 5. A frame that never
 * completes cannot let completed advance, so the ring was never stalled. The
 * shape is two frames posted and one already completed by the NHI interrupt
 * path (ring_work), which is what drains ~99.5% of completions on this
 * hardware -- the supplemental poll sees only the tail.
 *
 * Posts two frames, delivers one completion the way ring_work does (straight
 * into frame->callback from a context the driver's poll never runs in), then
 * runs the real poll @passes times with the last-progress stamp aged by
 * @stall_ms per pass. Reports whether the path reads as stalled to the rail
 * health gate, which is what decides if a new QP may bind this rail at all.
 */
int tbv_test_path_tx_interrupt_progress(u32 passes, u32 stall_ms,
					bool *tx_stalled_out,
					u64 *poll_completed_out,
					u32 *inflight_out)
{
	struct tbv_test_queue_timeout_ctx ctx = {};
	struct tbv_tx_packet *packets[2] = {};
	struct tbv_test_ring_dma *dma = NULL;
	struct tbv_data_frame *frames = NULL;
	struct tb_ring *ring = NULL;
	struct tbv_path *path = NULL;
	uint saved_poll_ms;
	uint saved_timeout_ms;
	u32 pass;
	int ret;

	path = kzalloc(sizeof(*path), GFP_KERNEL);
	frames = kcalloc(2, sizeof(*frames), GFP_KERNEL);
	ring = kzalloc(sizeof(*ring), GFP_KERNEL);
	dma = kzalloc(sizeof(*dma), GFP_KERNEL);
	if (!path || !frames || !ring || !dma) {
		ret = -ENOMEM;
		goto out;
	}

	ret = tbv_test_path_post_frames(path, ring, dma, frames, 2, packets,
					&ctx);
	if (ret)
		goto out;

	saved_poll_ms = tx_poll_delay_ms;
	saved_timeout_ms = tx_queue_timeout_ms;
	tx_poll_delay_ms = tx_poll_delay_ms ?: 1;
	/* No ceiling: this path is healthy and nothing here may be expired. */
	tx_queue_timeout_ms = 0;

	/*
	 * Age the path first, then complete: this is a path that has been busy
	 * for longer than the health gate's window with every completion taken
	 * by the interrupt handler.
	 */
	WRITE_ONCE(path->tx_last_progress_jiffies,
		   READ_ONCE(path->tx_last_progress_jiffies) -
			   msecs_to_jiffies(stall_ms));
	frames[0].frame.callback(ring, &frames[0].frame, false);
	packets[0] = NULL;

	for (pass = 0; pass < passes; pass++)
		tbv_path_tx_poll_work(&path->tx_poll_work.work);

	tx_poll_delay_ms = saved_poll_ms;
	tx_queue_timeout_ms = saved_timeout_ms;

	if (tx_stalled_out)
		*tx_stalled_out = tbv_path_tx_stalled(path);
	if (poll_completed_out)
		*poll_completed_out = atomic64_read(&path->tx_poll_completed);
	if (inflight_out)
		*inflight_out = (u32)max(atomic_read(&path->tx_inflight), 0);
	ret = 0;

out:
	if (path)
		tbv_test_path_teardown(path, packets, 2);
	kfree(dma);
	kfree(ring);
	kfree(frames);
	kfree(path);
	return ret;
}

/*
 * Scenario hook for tests/tx_ring_progress_test.c -- what the ring-frame
 * ceiling leaves behind when the ring recovers.
 *
 * tbv_path_expire_tx_ring_frames() deliberately does not return the frame to
 * tx_free, because the ring still owns the buffer and may complete it later.
 * That later completion is a real event: ring_work delivers every posted
 * descriptor eventually, canceled at worst. Both paths account the same
 * inflight slot, so the counter has to survive the pair.
 *
 * Posts one frame, ages it past @timeout_ms so the poll's ceiling releases it,
 * then delivers the ring completion. Reports the signed tx_inflight: below
 * zero, tbv_path_schedule_tx() reads it into a u32 and refuses every staged
 * data frame for the life of the path.
 */
int tbv_test_path_tx_expired_frame_completes(u32 timeout_ms, int *inflight_out,
					     u32 *done_calls_out,
					     int *status_out)
{
	struct tbv_test_queue_timeout_ctx ctx = {};
	struct tbv_tx_packet *packets[1] = {};
	struct tbv_test_ring_dma *dma = NULL;
	struct tbv_data_frame *frames = NULL;
	struct tb_ring *ring = NULL;
	struct tbv_path *path = NULL;
	uint saved_timeout_ms;
	uint saved_poll_ms;
	unsigned long flags;
	int ret;

	path = kzalloc(sizeof(*path), GFP_KERNEL);
	frames = kcalloc(1, sizeof(*frames), GFP_KERNEL);
	ring = kzalloc(sizeof(*ring), GFP_KERNEL);
	dma = kzalloc(sizeof(*dma), GFP_KERNEL);
	if (!path || !frames || !ring || !dma) {
		ret = -ENOMEM;
		goto out;
	}

	ret = tbv_test_path_post_frames(path, ring, dma, frames, 1, packets,
					&ctx);
	if (ret)
		goto out;

	saved_timeout_ms = tx_queue_timeout_ms;
	saved_poll_ms = tx_poll_delay_ms;
	tx_queue_timeout_ms = timeout_ms;
	tx_poll_delay_ms = tx_poll_delay_ms ?: 1;

	spin_lock_irqsave(&path->tx_lock, flags);
	frames[0].posted_jiffies -= msecs_to_jiffies(timeout_ms * 2);
	spin_unlock_irqrestore(&path->tx_lock, flags);
	tbv_path_tx_poll_work(&path->tx_poll_work.work);

	/* The ring completes the frame it still owned. */
	frames[0].frame.callback(ring, &frames[0].frame, false);
	packets[0] = NULL;

	tx_queue_timeout_ms = saved_timeout_ms;
	tx_poll_delay_ms = saved_poll_ms;

	if (inflight_out)
		*inflight_out = atomic_read(&path->tx_inflight);
	if (done_calls_out)
		*done_calls_out = ctx.done_calls;
	if (status_out)
		*status_out = ctx.last_status;
	ret = 0;

out:
	if (path)
		tbv_test_path_teardown(path, packets, 1);
	kfree(dma);
	kfree(ring);
	kfree(frames);
	kfree(path);
	return ret;
}

/*
 * Scenario hook for tests/tx_ring_progress_test.c -- an unframed window whose
 * remaining packets are cancelled out from under it.
 *
 * A read response is queued as a raw stream owned by its read context, and
 * tbv_cancel_read_ctx_packets() cancels it by (done, done_ctx) with no
 * owner_ctx. The window is opened by the header's dequeue and closed only by
 * the end packet's dequeue, so a cancel that takes the rest of the window
 * leaves it open with an owner that has nothing left to send.
 *
 * Builds that state on a real path: one queued packet owned by @A, one owned
 * by @B behind it, and the window open on @A exactly as posting @A's header
 * leaves it. Cancels @A's packets by done_ctx, then reports whether the window
 * is still open and how many packets are still queued -- with the window open
 * on an owner that has no queued packet, tbv_path_schedule_tx() refuses the
 * queue head forever and the path never sends again.
 */
int tbv_test_path_cancel_orphans_raw_window(bool *window_open_out,
					    u32 *queued_out,
					    u32 *done_calls_out)
{
	struct tbv_test_queue_timeout_ctx ctx_a = {};
	struct tbv_test_queue_timeout_ctx ctx_b = {};
	struct tbv_path_config cfg;
	struct tbv_path *path;
	u8 payload[64] = {};
	unsigned long flags;
	int ret;

	path = kzalloc(sizeof(*path), GFP_KERNEL);
	if (!path)
		return -ENOMEM;

	tbv_path_default_config(TBV_BACKEND_NATIVE, &cfg);
	tbv_path_init(path, &cfg, NULL);
	path->state = TBV_PATH_TUNNEL_ENABLED;
	path->tx_data_queue_limit = 8;
	/*
	 * No credits, so nothing leaves the queue: the window state below is
	 * built by hand, which is what a posted header leaves behind anyway.
	 */
	path->tx_remote_data_credit_max = 4;
	path->tx_remote_data_credits = 0;

	ret = tbv_path_send(path, payload, sizeof(payload), 0,
			    tbv_test_queue_timeout_done, &ctx_a);
	if (ret)
		goto out;
	ret = tbv_path_send(path, payload, sizeof(payload), 0,
			    tbv_test_queue_timeout_done, &ctx_b);
	if (ret)
		goto out;

	/* The state tbv_path_count_raw_stream_locked() leaves on a header. */
	spin_lock_irqsave(&path->tx_lock, flags);
	path->tx_raw_stream_active = true;
	path->tx_raw_stream_owner = &ctx_a;
	path->tx_raw_stream_window_open = true;
	path->tx_raw_stream_end_seen = false;
	path->tx_raw_stream_inflight = 1;
	spin_unlock_irqrestore(&path->tx_lock, flags);

	tbv_path_cancel_data_done_ctx(path, tbv_test_queue_timeout_done,
				      &ctx_a);

	spin_lock_irqsave(&path->tx_lock, flags);
	if (window_open_out)
		*window_open_out = path->tx_raw_stream_window_open;
	if (queued_out)
		*queued_out = path->tx_data_queued;
	spin_unlock_irqrestore(&path->tx_lock, flags);
	if (done_calls_out)
		*done_calls_out = ctx_a.done_calls;
	ret = 0;

out:
	path->state = TBV_PATH_STOPPED;
	cancel_delayed_work_sync(&path->tx_poll_work);
	tbv_path_flush_tx_queue(path, -ECANCELED);
	kfree(path);
	return ret;
}

int tbv_test_path_control_queue_bound(u32 extra, u32 *capacity_out,
				      u32 *accepted_out, u32 *queued_out)
{
	struct tbv_path_config cfg;
	struct tbv_path *path;
	u8 payload[32] = {};
	u32 capacity;
	u32 accepted = 0;
	u32 attempts;
	u32 i;
	int ret = 0;

	path = kzalloc(sizeof(*path), GFP_KERNEL);
	if (!path)
		return -ENOMEM;

	tbv_path_default_config(TBV_BACKEND_NATIVE, &cfg);
	tbv_path_init(path, &cfg, NULL);
	ret = tbv_path_alloc_control_packets(path);
	if (ret)
		goto out;

	path->state = TBV_PATH_TUNNEL_ENABLED;
	capacity = path->tx_control_packet_count;
	attempts = capacity + extra;

	/*
	 * No TX frames or ring are installed, so schedule_tx cannot dequeue a
	 * control packet. This is the producible state left by a ring that has
	 * stopped completing while RX continues to generate ACKs and credits.
	 */
	for (i = 0; i < attempts; i++) {
		ret = tbv_path_enqueue_control(path, payload, sizeof(payload),
					       NULL, NULL);
		if (ret == -ENOMEM) {
			ret = 0;
			break;
		}
		if (ret)
			goto out;
		accepted++;
	}

	if (capacity_out)
		*capacity_out = capacity;
	if (accepted_out)
		*accepted_out = accepted;
	if (queued_out)
		*queued_out = path->tx_control_queued;

out:
	path->state = TBV_PATH_STOPPED;
	tbv_path_flush_tx_queue(path, -ECANCELED);
	tbv_path_free_control_packets(path);
	kfree(path);
	return ret;
}

int tbv_test_path_retransmit_priority(u32 *first_out, u32 *second_out,
				      u32 *third_out)
{
	struct tbv_tx_packet initial_a = {};
	struct tbv_tx_packet initial_b = {};
	struct tbv_tx_packet retry = {};
	struct tbv_tx_packet *packet;
	struct tbv_path *path;
	LIST_HEAD(initial);
	LIST_HEAD(recovery);
	u32 order[3] = {};
	u32 i = 0;
	int ret;

	path = kzalloc(sizeof(*path), GFP_KERNEL);
	if (!path)
		return -ENOMEM;
	spin_lock_init(&path->tx_lock);
	INIT_LIST_HEAD(&path->tx_data_queue);
	path->state = TBV_PATH_TUNNEL_ENABLED;
	path->tx_data_queue_limit = 3;
	path->tx_data_reserved = 3;

	INIT_LIST_HEAD(&initial_a.node);
	INIT_LIST_HEAD(&initial_b.node);
	INIT_LIST_HEAD(&retry.node);
	initial_a.len = 1;
	initial_b.len = 2;
	retry.len = 3;
	list_add_tail(&initial_a.node, &initial);
	list_add_tail(&initial_b.node, &initial);
	list_add_tail(&retry.node, &recovery);

	/*
	 * Exercise the production reserved-enqueue primitive twice: ordinary
	 * traffic first, then recovery traffic. No ring is installed, so the
	 * scheduler cannot consume the queue while its order is inspected.
	 */
	ret = tbv_path_enqueue_reserved_data_list(path, &initial, 2, true,
						  false);
	if (ret)
		goto out;
	ret = tbv_path_enqueue_reserved_data_list(path, &recovery, 1, true,
						  true);
	if (ret)
		goto out;

	list_for_each_entry(packet, &path->tx_data_queue, node) {
		if (i >= ARRAY_SIZE(order)) {
			ret = -EOVERFLOW;
			goto out;
		}
		order[i++] = packet->len;
	}
	if (i != ARRAY_SIZE(order)) {
		ret = -EIO;
		goto out;
	}

	if (first_out)
		*first_out = order[0];
	if (second_out)
		*second_out = order[1];
	if (third_out)
		*third_out = order[2];
	ret = 0;
out:
	kfree(path);
	return ret;
}
#endif

void tbv_path_destroy(struct tbv_path *path, struct tb_xdomain *xd)
{
	bool tunnel_enabled = path->state == TBV_PATH_TUNNEL_ENABLED;
	bool rings_started = tunnel_enabled ||
			     path->state == TBV_PATH_RING_STARTED;

	/*
	 * tb_ring_stop() spins on nhi->lock with IRQs off and then
	 * flush_work()s the ring worker; tb_ring_free() sleeps too. Calling
	 * this from atomic context, under a spinlock shared with the ring
	 * completion path, or from a ring frame callback deadlocks the node
	 * (the 2026-06-26 chain-wide hard lock rode a teardown reached from
	 * forbidden context). Enforce the contract loudly instead of relying
	 * on every caller remembering it.
	 */
	might_sleep();

	if (rings_started) {
		/* tbv_path_quiesce() may already have stopped both rings. */
		if (!path->rings_fenced) {
			if (path->rx_ring)
				tb_ring_stop(path->rx_ring);
			if (path->tx_ring)
				tb_ring_stop(path->tx_ring);
		}
		path->state = TBV_PATH_RING_ALLOCATED;
	}
	cancel_delayed_work_sync(&path->tx_poll_work);
	cancel_delayed_work_sync(&path->rx_supp_poll_work);
	cancel_delayed_work_sync(&path->credit_sync_work);

	if (tunnel_enabled) {
		if (destroy_disable_paths) {
			tb_xdomain_disable_paths(xd, path->local_transmit_path,
						 path->local_tx_hop,
						 path->remote_transmit_path,
						 path->local_rx_hop);
		} else {
			pr_warn("skipping tb_xdomain_disable_paths route=0x%llx rail=0x%x out_hop=%d remote_out_hop=%d tx_hop=%d rx_hop=%d\n",
				path->rail && path->rail->peer ?
					path->rail->peer->xd->route : 0,
				path->rail ? path->rail->rail_id : 0xffffffff,
				path->local_transmit_path,
				path->remote_transmit_path,
				path->local_tx_hop, path->local_rx_hop);
		}
		tb_xdomain_release_in_hopid(xd, path->remote_transmit_path);
		path->remote_transmit_path = -1;
		path->state = TBV_PATH_RING_ALLOCATED;
	}
	if (!tunnel_enabled && path->remote_transmit_path >= 0) {
		tb_xdomain_release_in_hopid(xd, path->remote_transmit_path);
		path->remote_transmit_path = -1;
	}

	tbv_path_flush_tx_queue(path, -ECANCELED);

	if (path->rx_ring) {
		tbv_path_free_frames(path, false);
		tb_ring_free(path->rx_ring);
		path->rx_ring = NULL;
		path->local_rx_hop = -1;
	}

	if (path->local_transmit_path >= 0) {
		tb_xdomain_release_out_hopid(xd, path->local_transmit_path);
		path->local_transmit_path = -1;
	}

	if (path->tx_ring) {
		tbv_path_free_frames(path, true);
		tb_ring_free(path->tx_ring);
		path->tx_ring = NULL;
		path->local_tx_hop = -1;
	}
	tbv_path_free_data_packets(path);
	tbv_path_free_control_packets(path);

	path->state = TBV_PATH_STOPPED;
}
