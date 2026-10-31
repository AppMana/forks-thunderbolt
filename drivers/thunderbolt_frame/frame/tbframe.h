/* SPDX-License-Identifier: GPL-2.0 */
/*
 * tbframe - lossless frame service over Thunderbolt/USB4 XDomain DMA rings.
 *
 * This header is the complete contract between the link layer and its
 * clients (the tbrxe RDMA engine). It is the code form of
 * docs/tbframe-tbrxe-wire-spec.md; on any conflict the spec document is
 * updated, never silently diverged from.
 *
 * Layering rules (normative):
 *  - tbframe owns ALL hardware access: rings, HopIDs, tunnels, XDomain
 *    control messages, lane state. Clients never touch tb_* symbols.
 *  - Clients own everything from the first payload byte of a data frame.
 *    tbframe never parses payload contents.
 *  - No upcall is invoked with tbframe locks held.
 *  - Every downcall is non-blocking and callable from client task context.
 *  - Every internal tbframe wait on hardware is bounded; a timeout poisons
 *    the link (-> TBFRAME_DOWN_DEAD_HW), it never blocks the caller.
 *  - Recovery escalation stops at session re-handshake. tbframe never
 *    unloads the thunderbolt core, never resets the NHI.
 */

#ifndef TBFRAME_H
#define TBFRAME_H

#include <linux/types.h>

struct tbframe_link;

/* Hardware frame ceiling: NHI ring descriptors carry a 12-bit length. */
#define TBFRAME_MAX_FRAME	4096

/*
 * PDF (sof/eof) nibble values. The NHI filters RX frames by these masks;
 * a frame type is a PDF value, never a header field. 0x1/0x2 are tbnet's,
 * 0xf is the control channel convention; both are deliberately avoided.
 *
 * On the wire a frame is segmented into transport packets; the TX
 * descriptor's SOF PDF marks the first packet and the EOF PDF the last,
 * and the receiving NHI closes a frame on any packet whose PDF matches the
 * RX ring's eof_mask. The SOF marker therefore MUST be distinct from every
 * frame-type (EOF) value or multi-packet frames are chopped at the first
 * packet boundary (the tbnet FRAME_START/FRAME_END split exists for the
 * same reason). The frame type rides in the EOF PDF: single-packet frames
 * carry only that nibble, and the RX descriptor reports only the closing
 * PDF (sof reads back 0).
 */
#define TBFRAME_PDF_DATA	0x4	/* client payload frame (EOF)      */
#define TBFRAME_PDF_KEEPALIVE	0x5	/* 8-byte session-cookie probe (EOF) */
#define TBFRAME_PDF_SOF		0x6	/* start-of-frame marker, wire only */

/*
 * Mode A flow control: the per-link admission window is the peer's
 * advertised RX ring size minus this reserve, which is left for small
 * client control frames (ACKs) so bulk data can never starve them.
 */
#define TBFRAME_CTRL_RESERVE	64

/**
 * struct tbframe_frame - one transmit or receive frame
 * @data:    CPU address of the payload buffer (tbframe-owned memory)
 * @len:     payload length in bytes; on TX set by the client before
 *           tbframe_xmit(), on RX set by tbframe from the descriptor
 * @pdf:     frame type nibble (TBFRAME_PDF_*)
 * @is_ctrl: TX only - client marks small control frames (e.g. ACKs) so
 *           admission charges them to the control reserve, not the data
 *           window
 *
 * Frames are allocated and freed only by tbframe (tbframe_alloc_frame /
 * tbframe_frame_free); a frame handed to tbframe_xmit() is consumed on
 * success. RX frames delivered through rx() are valid only for the
 * duration of the callback unless the client takes them with
 * tbframe_frame_get_rx(); it must then return them with
 * tbframe_frame_put_rx() so the descriptor can be reposted.
 */
struct tbframe_frame {
	void	*data;
	u16	len;
	u8	pdf;
	bool	is_ctrl;
	/* private: tbframe internal fields follow in the implementation */
};

/**
 * enum tbframe_down_reason - why a link left the UP state
 * @TBFRAME_DOWN_UNPLUG:     hotplug removal / service unbind
 * @TBFRAME_DOWN_LOGOUT:     orderly LOGOUT from the peer
 * @TBFRAME_DOWN_SUPERSEDE:  peer re-HELLOed while established (peer reboot
 *                           inside one verify interval, or cookie mismatch)
 * @TBFRAME_DOWN_VERIFY:     level-triggered paths_active read-back failed
 * @TBFRAME_DOWN_CLOSED:     local tbframe_close()
 * @TBFRAME_DOWN_DEAD_HW:    bounded hardware wait timed out; link poisoned
 *
 * Loss-model contract: frames are never silently lost while a link is UP;
 * every loss window is bracketed by link_down() -> link_up() and a session
 * reset. Clients (the PSN layer) recover across that bracket.
 */
enum tbframe_down_reason {
	TBFRAME_DOWN_UNPLUG,
	TBFRAME_DOWN_LOGOUT,
	TBFRAME_DOWN_SUPERSEDE,
	TBFRAME_DOWN_VERIFY,
	TBFRAME_DOWN_CLOSED,
	TBFRAME_DOWN_DEAD_HW,
};

/**
 * struct tbframe_link_info - session attributes valid while the link is UP
 * @gid_eui64:    peer's per-link ULA EUI-64 from HELLO (GID derivation)
 * @local_gid_eui64: the EUI-64 THIS side advertised in its own HELLO on
 *                this link. The client must publish GIDs derived from this
 *                identity (not one of its own making), so the GID a peer
 *                derives from our HELLO and the GID we hand to userspace
 *                agree end to end.
 * @rx_ring_entries: peer's advertised RX ring size (Mode A window basis)
 * @data_window:  admitted concurrent data frames (ring entries minus
 *                reserve in Mode A; larger when Mode B/E2E is active)
 * @max_payload:  largest payload tbframe_alloc_frame() will grant
 * @e2e:          hardware E2E credits active on this link (Mode B)
 * @width:        link width (lanes)
 * @speed:        per-lane speed in Gb/s
 * @remote_uuid:  peer host router UUID (zeros when unknown). Stable
 *                hardware identity for per-link provisioning (the
 *                tbv-rdma-addr deterministic ULA scheme hashes the sorted
 *                endpoint UUID pair).
 * @remote_name:  peer hostname from the XDomain properties ("" when
 *                unknown); used for stable netdev naming (tbr-<peer>).
 * @route:        XDomain route of this link's port. On an intra-domain
 *                self-loop (both ports of one host cabled together) every
 *                UUID is equal on both ends and the route is the ONLY
 *                per-end-distinct value; the udev self-loop addressing
 *                and naming tie-breaks derive from it.
 */
struct tbframe_link_info {
	u64	gid_eui64;
	u64	local_gid_eui64;
	u64	route;
	u16	rx_ring_entries;
	u16	data_window;
	u16	max_payload;
	bool	e2e;
	u8	width;
	u8	speed;
	u8	remote_uuid[16];
	char	remote_name[48];
};

/**
 * struct tbframe_client_ops - upcalls into the client
 * @rx:          one received frame. Called from tbframe's NAPI-style poll
 *               context in bounded batches, never with tbframe locks held.
 *               The frame is only valid during the call (see
 *               struct tbframe_frame).
 * @rx_bad:      optional diagnostic-only view of a frame rejected by the NHI
 *               RX descriptor status. It is synchronous and read-only, and
 *               the frame is recycled immediately afterwards. A bad frame is
 *               never also delivered through @rx.
 * @tx_released: the admission window reopened (completions drained below
 *               the watermark). The client's send machinery should
 *               reschedule. May coalesce many completions into one call.
 * @link_up:     the session reached READY in both directions; @info is
 *               valid until link_down. GIDs derived from it become valid.
 * @link_down:   the session left UP for @reason. All frames in flight are
 *               gone (completed with error or cancelled); all admission
 *               windows reset; GIDs from this link are invalid. tbframe
 *               re-handshakes automatically for every reason except
 *               TBFRAME_DOWN_CLOSED and TBFRAME_DOWN_DEAD_HW.
 *
 * All upcalls receive the @ctx registered in tbframe_register_client().
 */
struct tbframe_client_ops {
	void (*rx)(void *ctx, struct tbframe_link *link,
		   struct tbframe_frame *frame);
	void (*rx_bad)(void *ctx, struct tbframe_link *link,
		       struct tbframe_frame *frame);
	void (*tx_released)(void *ctx, struct tbframe_link *link);
	void (*link_up)(void *ctx, struct tbframe_link *link,
			const struct tbframe_link_info *info);
	void (*link_down)(void *ctx, struct tbframe_link *link,
			  enum tbframe_down_reason reason);
};

/*
 * Client registration. One client (the RDMA engine) per module instance;
 * links are discovered by tbframe (XDomain service probe + HELLO) and
 * announced via link_up(). Unregistering closes every link first
 * (link_down(TBFRAME_DOWN_CLOSED) is delivered for each) and returns only
 * when no upcall is running or can run again.
 */
int tbframe_register_client(const struct tbframe_client_ops *ops, void *ctx);
void tbframe_unregister_client(void);

/*
 * Downcalls. All are non-blocking.
 *
 * tbframe_alloc_frame(): grab a TX frame with room for @len payload bytes.
 *   Fails with -ENOSPC when the admission window (data or control,
 *   per @is_ctrl) is exhausted - the client stops producing and waits for
 *   tx_released(). Fails with -ENETDOWN when the link is not UP.
 *
 * tbframe_xmit(): queue the frame; consumes it on success (completion
 *   releases the window and may trigger tx_released()). On failure the
 *   frame remains owned by the caller (free or retry). Frames on one link
 *   complete strictly in order.
 *
 * tbframe_frame_free(): return an un-transmitted TX frame.
 */
int tbframe_alloc_frame(struct tbframe_link *link, u16 len, bool is_ctrl,
			struct tbframe_frame **frame);
int tbframe_xmit(struct tbframe_link *link, struct tbframe_frame *frame);
void tbframe_frame_free(struct tbframe_link *link,
			struct tbframe_frame *frame);

/* RX frame retention across the rx() callback (bounded by ring depth). */
void tbframe_frame_get_rx(struct tbframe_frame *frame);
void tbframe_frame_put_rx(struct tbframe_link *link,
			  struct tbframe_frame *frame);

/* Identity and diagnostics. */
const char *tbframe_link_name(const struct tbframe_link *link);
void tbframe_link_info(const struct tbframe_link *link,
		       struct tbframe_link_info *info);

#endif /* TBFRAME_H */
