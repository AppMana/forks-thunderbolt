/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
#ifndef TBV_NATIVE_DATA_H
#define TBV_NATIVE_DATA_H

#include "native_wire.h"

#define TBV_NATIVE_DATA_MAGIC		0x31445654u /* "TVD1" little-endian */
#define TBV_NATIVE_DATA_VERSION		1u
#define TBV_NATIVE_DATA_HDR_SIZE	48u
#define TBV_NATIVE_DATA_FRAME_SIZE	4096u
#define TBV_NATIVE_DATA_MAX_PAYLOAD \
	(TBV_NATIVE_DATA_FRAME_SIZE - TBV_NATIVE_DATA_HDR_SIZE)
#define TBV_NATIVE_DATA_MAX_MSG_SIZE	(16u * 1024u * 1024u)
#define TBV_NATIVE_DATA_CREDIT_BATCH	32u
#define TBV_NATIVE_DATA_MAX_FRAGS \
	((TBV_NATIVE_DATA_MAX_MSG_SIZE + TBV_NATIVE_DATA_MAX_PAYLOAD - 1u) / \
	 TBV_NATIVE_DATA_MAX_PAYLOAD)

enum tbv_native_data_op {
	TBV_NATIVE_DATA_OP_SEND = 1,
	TBV_NATIVE_DATA_OP_SEND_ACK = 2,
	TBV_NATIVE_DATA_OP_RDMA_WRITE = 3,
	TBV_NATIVE_DATA_OP_RDMA_WRITE_IMM = 4,
	TBV_NATIVE_DATA_OP_RECV_CREDIT = 5,
	TBV_NATIVE_DATA_OP_SEND_IMM = 6,
	TBV_NATIVE_DATA_OP_RDMA_READ_REQ = 7,
	TBV_NATIVE_DATA_OP_RDMA_READ_RESP = 8,
	TBV_NATIVE_DATA_OP_PATH_CREDIT = 9,
	TBV_NATIVE_DATA_OP_RDMA_READ_ACK = 10,
	TBV_NATIVE_DATA_OP_MAD = 11,
	/*
	 * Selective retransmit request (requires TBV_NATIVE_WIRE_CAP_NAK from
	 * the peer's HELLO). psn names the message, frag_offset the first
	 * missing byte, imm_data the missing byte count (0 = through the end
	 * of the message). length/remote_addr/rkey/flags are reserved zero.
	 * frag_offset also serves as a cumulative SACK: every byte below it
	 * has been received, so the sender may drop those fragments from any
	 * later timeout retransmit.
	 */
	TBV_NATIVE_DATA_OP_NAK = 12,
	/*
	 * Absolute re-advertisement of the data credits the sender has returned
	 * on this path since the last capacity negotiation (requires
	 * TBV_NATIVE_WIRE_CAP_CREDIT_SYNC from the peer's HELLO). frag_offset
	 * carries that cumulative count, wrapping; every other field is
	 * reserved zero. PATH_CREDIT carries a delta and is unacknowledged, so
	 * one dropped credit frame is a permanent shortfall in the peer's
	 * window; the cumulative count lets the peer recompute the exact
	 * shortfall from any single later frame.
	 */
	TBV_NATIVE_DATA_OP_PATH_CREDIT_SYNC = 13,
	TBV_NATIVE_DATA_OP_MAX = TBV_NATIVE_DATA_OP_PATH_CREDIT_SYNC,
};

enum tbv_native_data_flag {
	TBV_NATIVE_DATA_F_LAST = 1u << 0,
	TBV_NATIVE_DATA_F_SOLICITED = 1u << 1,
	TBV_NATIVE_DATA_F_RAW_STREAM = 1u << 2,
};

enum tbv_native_read_ack_status {
	TBV_NATIVE_READ_ACK_OK = 0,
	TBV_NATIVE_READ_ACK_RETRY = 1,
	TBV_NATIVE_READ_ACK_ERROR = 2,
};

enum tbv_native_send_ack_status {
	TBV_NATIVE_SEND_ACK_OK = 0,
	TBV_NATIVE_SEND_ACK_RNR = 1,
	TBV_NATIVE_SEND_ACK_ERROR = 2,
};

struct tbv_native_data_header {
	tbv_wire_u8 opcode;
	tbv_wire_u8 flags;
	tbv_wire_u32 dest_qp;
	tbv_wire_u32 src_qp;
	tbv_wire_u32 psn;
	/*
	 * length is this fragment's payload bytes. frag_offset is this
	 * fragment's byte offset within the operation. For SEND/SEND_IMM,
	 * imm_data is total message bytes, remote_addr is zero, and rkey carries
	 * immediate data for SEND_IMM. For RDMA_WRITE/RDMA_WRITE_IMM,
	 * remote_addr is the base remote IOVA, rkey is the remote key, and
	 * imm_data carries immediate data only for RDMA_WRITE_IMM. For
	 * SEND_ACK, imm_data is enum tbv_native_send_ack_status.
	 */
	tbv_wire_u32 length;
	tbv_wire_u32 imm_data;
	tbv_wire_u64 remote_addr;
	tbv_wire_u32 rkey;
	tbv_wire_u32 frag_offset;
};

/*
 * The receiver returns path data-credits batched at this threshold; a
 * sub-threshold remainder is held until more frames arrive (in a strict
 * 1-in-flight ping-pong it is never flushed). The peer can therefore withhold
 * up to (threshold - 1) credits indefinitely, while a multi-frame sender must
 * acquire min(frames, threshold) credits to START its group
 * (tbv_native_data_start_credit_required). For the sender never to deadlock the
 * credits the peer can NEVER withhold must still cover one start:
 *     window - (threshold - 1) >= min(frames, threshold).
 * Capping the threshold at half the window guarantees this for every message a
 * window can hold (window - (threshold-1) >= threshold >= start). The old
 * min(window, BATCH) returned `window` for window < BATCH -- withholding
 * window-1 and stranding any multi-frame send (16K ib_send_lat = 5 frames hangs;
 * 64B = 1 frame never needs >1 credit and is immune). See
 * tests/credit_pingpong_test.c.
 */
static inline tbv_wire_u32
tbv_native_data_credit_return_threshold(tbv_wire_u32 credit_window)
{
	tbv_wire_u32 threshold = TBV_NATIVE_DATA_CREDIT_BATCH;

	if (!credit_window)
		return threshold;
	if (threshold > (credit_window + 1u) / 2u)
		threshold = (credit_window + 1u) / 2u;
	if (!threshold)
		threshold = 1u;
	return threshold;
}

/*
 * New credit balance after refunding @frames of a failed send attempt, clamped
 * to @max. Used before a retransmit re-charges the frames so a lost frame's
 * orphaned credit is reclaimed; the clamp absorbs the over-refund for frames
 * that did arrive (the peer already returned those). See path.c and
 * tests/credit_pingpong_test.c.
 */
static inline tbv_wire_u32
tbv_native_data_refund_credits(tbv_wire_u32 credits, tbv_wire_u32 max,
			       tbv_wire_u32 frames)
{
	if (!max)
		return credits;
	if (credits >= max || frames > max - credits)
		return max;
	return credits + frames;
}

static inline tbv_wire_u32
tbv_native_data_start_credit_required(tbv_wire_u32 frames,
				      tbv_wire_u32 credit_window)
{
	tbv_wire_u32 threshold;

	if (!frames)
		return 0;

	threshold = tbv_native_data_credit_return_threshold(credit_window);
	return frames < threshold ? frames : threshold;
}

static inline int
tbv_native_data_build_header(void *buf, size_t size,
			     const struct tbv_native_data_header *hdr)
{
	tbv_wire_u8 *p = buf;

	if (!p || !hdr)
		return -EINVAL;
	if (size < TBV_NATIVE_DATA_HDR_SIZE)
		return -ENOSPC;
	if (!hdr->opcode || hdr->opcode > TBV_NATIVE_DATA_OP_MAX)
		return -EINVAL;
	if (hdr->length > ((hdr->flags & TBV_NATIVE_DATA_F_RAW_STREAM) ?
			   TBV_NATIVE_DATA_MAX_MSG_SIZE :
			   TBV_NATIVE_DATA_MAX_PAYLOAD))
		return -EMSGSIZE;

	memset(p, 0, TBV_NATIVE_DATA_HDR_SIZE);
	tbv_wire_put_le32(p, TBV_NATIVE_DATA_MAGIC);
	tbv_wire_put_le16(p + 4, TBV_NATIVE_DATA_VERSION);
	tbv_wire_put_le16(p + 6, TBV_NATIVE_DATA_HDR_SIZE);
	p[8] = hdr->opcode;
	p[9] = hdr->flags;
	tbv_wire_put_le32(p + 12, hdr->dest_qp);
	tbv_wire_put_le32(p + 16, hdr->src_qp);
	tbv_wire_put_le32(p + 20, hdr->psn);
	tbv_wire_put_le32(p + 24, hdr->length);
	tbv_wire_put_le32(p + 28, hdr->imm_data);
	tbv_wire_put_le64(p + 32, hdr->remote_addr);
	tbv_wire_put_le32(p + 40, hdr->rkey);
	tbv_wire_put_le32(p + 44, hdr->frag_offset);

	return TBV_NATIVE_DATA_HDR_SIZE;
}

static inline int
tbv_native_data_parse_header(const void *buf, size_t size,
			     struct tbv_native_data_header *hdr)
{
	const tbv_wire_u8 *p = buf;
	tbv_wire_u16 version;
	tbv_wire_u16 header_size;

	if (!p || !hdr)
		return -EINVAL;
	if (size < TBV_NATIVE_DATA_HDR_SIZE)
		return -EINVAL;
	if (tbv_wire_get_le32(p) != TBV_NATIVE_DATA_MAGIC)
		return -EINVAL;

	version = tbv_wire_get_le16(p + 4);
	header_size = tbv_wire_get_le16(p + 6);
	if (version != TBV_NATIVE_DATA_VERSION ||
	    header_size != TBV_NATIVE_DATA_HDR_SIZE)
		return -EINVAL;

	memset(hdr, 0, sizeof(*hdr));
	hdr->opcode = p[8];
	hdr->flags = p[9];
	hdr->dest_qp = tbv_wire_get_le32(p + 12);
	hdr->src_qp = tbv_wire_get_le32(p + 16);
	hdr->psn = tbv_wire_get_le32(p + 20);
	hdr->length = tbv_wire_get_le32(p + 24);
	hdr->imm_data = tbv_wire_get_le32(p + 28);
	hdr->remote_addr = tbv_wire_get_le64(p + 32);
	hdr->rkey = tbv_wire_get_le32(p + 40);
	hdr->frag_offset = tbv_wire_get_le32(p + 44);

	if (!hdr->opcode || hdr->opcode > TBV_NATIVE_DATA_OP_MAX)
		return -EINVAL;
	if (hdr->length > ((hdr->flags & TBV_NATIVE_DATA_F_RAW_STREAM) ?
			   TBV_NATIVE_DATA_MAX_MSG_SIZE :
			   TBV_NATIVE_DATA_MAX_PAYLOAD))
		return -EMSGSIZE;

	return 0;
}

/*
 * Synthesize the per-fragment header for @payload_len raw bytes of a stream.
 * The stream header's frag_offset is the stream's base offset within the
 * operation: legacy full-message streams carry 0 there, per-fragment split
 * streams (TBV_NATIVE_WIRE_CAP_SPLIT_DATA) carry the fragment's byte offset,
 * so the synthesized offset is base + bytes already consumed.
 */
static inline int
tbv_native_data_raw_payload_header(const struct tbv_native_data_header *stream,
				   tbv_wire_u32 done,
				   tbv_wire_u32 remaining,
				   tbv_wire_u32 payload_len,
				   struct tbv_native_data_header *payload)
{
	if (!stream || !payload)
		return -EINVAL;
	if (!(stream->flags & TBV_NATIVE_DATA_F_RAW_STREAM))
		return -EINVAL;
	if (!payload_len || payload_len > remaining)
		return -EINVAL;

	*payload = *stream;
	payload->flags &= ~(TBV_NATIVE_DATA_F_RAW_STREAM |
			    TBV_NATIVE_DATA_F_LAST |
			    TBV_NATIVE_DATA_F_SOLICITED);
	if (payload_len == remaining)
		payload->flags |= stream->flags &
				  (TBV_NATIVE_DATA_F_LAST |
				   TBV_NATIVE_DATA_F_SOLICITED);
	payload->length = payload_len;
	payload->frag_offset = stream->frag_offset + done;
	return 0;
}

/*
 * Per-fragment split-stream framing (TBV_NATIVE_WIRE_CAP_SPLIT_DATA). A split
 * fragment covers one ring-frame-sized window of the operation so the payload
 * needs no bounce copy: the window maps to at most two user pages, each posted
 * as its own raw frame. The unit is the full 4096-byte frame (not the 4048
 * copied-path payload) because raw frames carry no header; the receive side's
 * write fragment shape already blesses both units.
 */
#define TBV_NATIVE_DATA_SPLIT_UNIT	TBV_NATIVE_DATA_FRAME_SIZE

static inline tbv_wire_u32 tbv_native_data_split_window_count(tbv_wire_u32 total_len)
{
	if (!total_len)
		return 1;
	return (total_len + TBV_NATIVE_DATA_SPLIT_UNIT - 1u) /
	       TBV_NATIVE_DATA_SPLIT_UNIT;
}

static inline int tbv_native_data_split_window(tbv_wire_u32 total_len,
					       tbv_wire_u32 idx,
					       tbv_wire_u32 *offset,
					       tbv_wire_u32 *len)
{
	tbv_wire_u32 count = tbv_native_data_split_window_count(total_len);

	if (!offset || !len || idx >= count)
		return -EINVAL;

	*offset = idx * TBV_NATIVE_DATA_SPLIT_UNIT;
	*len = idx == count - 1 ? total_len - *offset :
				  TBV_NATIVE_DATA_SPLIT_UNIT;
	return 0;
}

/*
 * PATH_CREDIT_SYNC header validation. Only frag_offset carries information (the
 * cumulative returned-credit count, which is legally zero before the path has
 * returned anything); every other field must be zero so they stay available for
 * later extension.
 */
static inline bool
tbv_native_data_valid_path_credit_sync(const struct tbv_native_data_header *hdr)
{
	return hdr->opcode == TBV_NATIVE_DATA_OP_PATH_CREDIT_SYNC &&
	       !hdr->flags &&
	       !hdr->dest_qp &&
	       !hdr->src_qp &&
	       !hdr->psn &&
	       !hdr->length &&
	       !hdr->imm_data &&
	       !hdr->remote_addr &&
	       !hdr->rkey;
}

/*
 * Credits to grant on an absolute resync. @seen is the cumulative count the
 * receiver has already applied, @total the count the peer reports. The
 * difference is exactly what was lost with a dropped PATH_CREDIT frame; it is
 * computed modulo 2^32 so the counter may wrap freely. The caller still clamps
 * the grant at the window maximum, which is what keeps a stale or duplicated
 * resync from ever letting the sender exceed the peer's ring depth (the same
 * property the retransmit refund relies on).
 */
static inline tbv_wire_u32
tbv_native_data_resync_delta(tbv_wire_u32 seen, tbv_wire_u32 total)
{
	return total - seen;
}

/*
 * NAK header validation. Only the psn / frag_offset / imm_data fields carry
 * information (see TBV_NATIVE_DATA_OP_NAK); everything else must be zero so
 * the fields stay available for later extension.
 */
static inline bool
tbv_native_data_valid_nak(const struct tbv_native_data_header *hdr)
{
	return hdr->opcode == TBV_NATIVE_DATA_OP_NAK &&
	       !hdr->flags &&
	       !hdr->length &&
	       !hdr->remote_addr &&
	       !hdr->rkey;
}

static inline int
tbv_native_data_fragment_shape(tbv_wire_u32 total_len,
			       tbv_wire_u32 max_payload,
			       tbv_wire_u32 max_frags,
			       tbv_wire_u32 offset,
			       tbv_wire_u32 len,
			       bool last,
			       tbv_wire_u32 *frag_idx,
			       tbv_wire_u32 *frag_count)
{
	tbv_wire_u32 idx;
	tbv_wire_u32 count;
	tbv_wire_u32 expected_len;

	if (!max_payload || !max_frags || !frag_idx || !frag_count)
		return -EINVAL;

	if (!total_len) {
		if (offset || len || !last)
			return -EINVAL;
		*frag_idx = 0;
		*frag_count = 1;
		return 0;
	}

	if (offset % max_payload)
		return -EINVAL;
	idx = offset / max_payload;
	count = (total_len + max_payload - 1u) / max_payload;
	if (idx >= count || count > max_frags)
		return -EINVAL;

	expected_len = idx == count - 1 ?
			      total_len - idx * max_payload : max_payload;
	if (len != expected_len)
		return -EINVAL;
	if (last != (idx == count - 1))
		return -EINVAL;

	*frag_idx = idx;
	*frag_count = count;
	return 0;
}

/*
 * Resolve an RDMA_WRITE fragment's (idx, count) against the two legal framing
 * units: 4048 (copied path) and 4096 (raw/split path). A fragment can validate
 * under BOTH when its offset is a multiple of lcm(4048,4096) (~1 MiB, e.g.
 * split window 253) with different indices; picking the wrong one used to be
 * impossible to hit (raw fragments were never buffered) but split fragments
 * reorder-buffer routinely. @known_frag_count disambiguates: when the message
 * already established its count, only the unit reproducing it is accepted.
 */
static inline int
tbv_native_data_write_shape_resolve(tbv_wire_u32 total_len,
				    tbv_wire_u32 max_frags,
				    tbv_wire_u32 offset,
				    tbv_wire_u32 len,
				    bool last,
				    tbv_wire_u32 known_frag_count,
				    tbv_wire_u32 *frag_idx,
				    tbv_wire_u32 *frag_count)
{
	static const tbv_wire_u32 units[2] = {
		TBV_NATIVE_DATA_MAX_PAYLOAD,
		TBV_NATIVE_DATA_FRAME_SIZE,
	};
	bool have_fallback = false;
	tbv_wire_u32 fb_idx = 0;
	tbv_wire_u32 fb_count = 0;
	unsigned int i;

	for (i = 0; i < 2; i++) {
		tbv_wire_u32 idx;
		tbv_wire_u32 count;

		if (tbv_native_data_fragment_shape(total_len, units[i],
						   max_frags, offset, len, last,
						   &idx, &count))
			continue;
		if (!known_frag_count || count == known_frag_count) {
			*frag_idx = idx;
			*frag_count = count;
			return 0;
		}
		if (!have_fallback) {
			have_fallback = true;
			fb_idx = idx;
			fb_count = count;
		}
	}

	/*
	 * Shape-valid under some unit but not the message's established count:
	 * report that shape so the caller's count-mismatch handling (reorder
	 * collision) sees it, instead of failing as a malformed fragment.
	 */
	if (have_fallback) {
		*frag_idx = fb_idx;
		*frag_count = fb_count;
		return 0;
	}
	return -EINVAL;
}

#endif /* TBV_NATIVE_DATA_H */
