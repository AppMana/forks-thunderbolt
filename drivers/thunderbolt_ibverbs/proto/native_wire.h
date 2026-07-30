/* SPDX-License-Identifier: GPL-2.0 */
#ifndef TBV_NATIVE_WIRE_H
#define TBV_NATIVE_WIRE_H

#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/types.h>
typedef u8 tbv_wire_u8;
typedef u16 tbv_wire_u16;
typedef u32 tbv_wire_u32;
typedef u64 tbv_wire_u64;
#else
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
typedef uint8_t tbv_wire_u8;
typedef uint16_t tbv_wire_u16;
typedef uint32_t tbv_wire_u32;
typedef uint64_t tbv_wire_u64;
#endif

#define TBV_NATIVE_WIRE_MAGIC		0x31564254u /* "TBV1" little-endian */
/*
 * v2: HELLO grew roce_eui64 + roce_ipv4 (the sender's RoCE GID identity) so a
 * multi-peer node can resolve which Thunderbolt peer a destination GID refers
 * to and rebind QPs to the right rail at RTR time (the QP is rail-bound at
 * create_qp, before the destination is known; on a mid-chain node with two
 * neighbours the initial bind is a coin flip). Parse rejects mismatched
 * versions, so a v1 and a v2 node simply never negotiate; roll the fleet
 * together (playbook_thunderbolt_ibverbs.yaml) and watch for the version
 * mismatch warn in dmesg.
 */
#define TBV_NATIVE_WIRE_VERSION		2u
#define TBV_NATIVE_WIRE_XDOMAIN_HDR_SIZE 32u
#define TBV_NATIVE_WIRE_HDR_SIZE	16u
#define TBV_NATIVE_WIRE_HELLO_SIZE	56u
#define TBV_NATIVE_WIRE_HELLO_MSG_SIZE \
	(TBV_NATIVE_WIRE_XDOMAIN_HDR_SIZE + TBV_NATIVE_WIRE_HDR_SIZE + \
	 TBV_NATIVE_WIRE_HELLO_SIZE)

static const tbv_wire_u8 tbv_native_wire_uuid[16] = {
	0x7c, 0x2c, 0x8f, 0x1e, 0x5b, 0x4d, 0x4a, 0x01,
	0x9f, 0x3a, 0x2b, 0x8e, 0x6d, 0x4c, 0x1a, 0x07,
};

enum tbv_native_wire_op {
	TBV_NATIVE_WIRE_OP_HELLO = 1,
	TBV_NATIVE_WIRE_OP_HELLO_ACK = 2,
	TBV_NATIVE_WIRE_OP_READY = 3,
	TBV_NATIVE_WIRE_OP_READY_ACK = 4,
};

/*
 * Capability bits are the additive negotiation channel: unknown bits are
 * ignored by older modules (caps are only ever tested with &), so new
 * data-plane behavior rides a new bit instead of a wire-version bump (which
 * would make mixed fleets stop negotiating entirely, see the v2 note above).
 * A sender may only emit the corresponding frames after the peer's HELLO
 * advertised the bit; absence degrades to the previous behavior.
 *
 * SPLIT_DATA: the peer accepts per-fragment raw streams -- a 48-byte native
 * data header frame with TBV_NATIVE_DATA_F_RAW_STREAM, length <= one ring
 * frame and a NONZERO frag_offset stream base, followed by that many raw
 * payload bytes. Pre-SPLIT_DATA receivers assume a zero stream base and would
 * scatter the payload at the wrong offsets.
 *
 * NAK: the peer understands TBV_NATIVE_DATA_OP_NAK (selective retransmit
 * request). Pre-NAK receivers count the opcode as data_rx_bad_header, so it
 * must never be sent to them; they recover by their retransmit timer alone.
 *
 * RAIL_EUI64: roce_eui64 is a SYNTHETIC per-rail identity (the sender has no
 * pinned roce_netdev): the modified-EUI-64 of the sender's rail netdev MAC
 * 02:H1:H2:H3:P:R, where H is a stable 24-bit host hash (host-unique) and
 * P:R are the sender's LOCAL peer_id/rail_id numbering. P:R mean nothing to
 * the receiver (every node numbers its peers independently) and differ across
 * the sender's rails, so the receiver must match destination-GID interface-ids
 * on the HOST part only (upper 48 bits, eui64 >> 16). That makes every GID of
 * the sender's node resolvable, whichever of its ib_devices it came from.
 * Without this bit roce_eui64 is a real (pinned) netdev identity and is
 * matched exactly, as before.
 */
enum tbv_native_wire_cap {
	TBV_NATIVE_WIRE_CAP_UC = 1u << 0,
	TBV_NATIVE_WIRE_CAP_RC = 1u << 1,
	TBV_NATIVE_WIRE_CAP_MULTI_RAIL = 1u << 2,
	TBV_NATIVE_WIRE_CAP_SPLIT_DATA = 1u << 3,
	TBV_NATIVE_WIRE_CAP_NAK = 1u << 4,
	TBV_NATIVE_WIRE_CAP_RAIL_EUI64 = 1u << 5,
	/*
	 * Peer understands TBV_NATIVE_DATA_OP_PATH_CREDIT_SYNC, the absolute
	 * re-advertisement of the data credits this side has returned. Without
	 * it the peer only ever hears the deltas in PATH_CREDIT, so a single
	 * dropped credit frame shortens its window permanently. Receive support
	 * is unconditional; the sender gates emission on this bit because an
	 * older peer rejects the opcode as a bad header.
	 */
	TBV_NATIVE_WIRE_CAP_CREDIT_SYNC = 1u << 6,
	/*
	 * Peer understands that PATH_CREDIT is redundant when both ends
	 * advertise hardware E2E for the native path. A sender may bypass the
	 * software window only when this bit and the peer's E2E path flag are
	 * both present; mixed-version and non-E2E paths retain PATH_CREDIT.
	 */
	TBV_NATIVE_WIRE_CAP_E2E_NO_SW_CREDIT = 1u << 7,
};

enum tbv_native_wire_path_flag {
	TBV_NATIVE_WIRE_PATH_FRAME = 1u << 0,
	TBV_NATIVE_WIRE_PATH_E2E = 1u << 1,
};

struct tbv_native_wire_info {
	tbv_wire_u16 op;
	tbv_wire_u16 flags;
	tbv_wire_u8 xdomain_sequence;
	tbv_wire_u32 seq;
	tbv_wire_u64 route;
};

struct tbv_native_wire_hello {
	tbv_wire_u32 capabilities;
	tbv_wire_u32 rail_id;
	tbv_wire_u64 route;
	tbv_wire_u32 tx_hop;
	tbv_wire_u32 rx_hop;
	tbv_wire_u32 transmit_path;
	tbv_wire_u32 tx_ring_size;
	tbv_wire_u32 rx_ring_size;
	tbv_wire_u32 path_flags;
	/*
	 * v2: the sender's RoCE GID identity, used by the receiver to map a
	 * destination GID back to this Thunderbolt peer. roce_eui64 is the
	 * EUI-64 the kernel derives from the sender's roce_netdev MAC (the
	 * low 8 bytes of its link-local / SLAAC GIDs, modified-EUI-64 with
	 * the U/L bit flipped); roce_ipv4 (network byte order) matches the
	 * IPv4-mapped ::ffff:a.b.c.d GID. Zero means "unknown" (no netdev).
	 */
	tbv_wire_u64 roce_eui64;
	tbv_wire_u32 roce_ipv4;
	tbv_wire_u32 reserved;
};

static inline void tbv_wire_put_le16(tbv_wire_u8 *p, tbv_wire_u16 value)
{
	p[0] = value & 0xffu;
	p[1] = value >> 8;
}

static inline void tbv_wire_put_le32(tbv_wire_u8 *p, tbv_wire_u32 value)
{
	p[0] = value & 0xffu;
	p[1] = (value >> 8) & 0xffu;
	p[2] = (value >> 16) & 0xffu;
	p[3] = value >> 24;
}

static inline void tbv_wire_put_le64(tbv_wire_u8 *p, tbv_wire_u64 value)
{
	tbv_wire_put_le32(p, value & 0xffffffffull);
	tbv_wire_put_le32(p + 4, value >> 32);
}

static inline tbv_wire_u16 tbv_wire_get_le16(const tbv_wire_u8 *p)
{
	return (tbv_wire_u16)p[0] | ((tbv_wire_u16)p[1] << 8);
}

static inline tbv_wire_u32 tbv_wire_get_le32(const tbv_wire_u8 *p)
{
	return (tbv_wire_u32)p[0] |
	       ((tbv_wire_u32)p[1] << 8) |
	       ((tbv_wire_u32)p[2] << 16) |
	       ((tbv_wire_u32)p[3] << 24);
}

static inline tbv_wire_u64 tbv_wire_get_le64(const tbv_wire_u8 *p)
{
	return (tbv_wire_u64)tbv_wire_get_le32(p) |
	       ((tbv_wire_u64)tbv_wire_get_le32(p + 4) << 32);
}

static inline void tbv_native_wire_put_header(tbv_wire_u8 *p,
					      tbv_wire_u16 op,
					      tbv_wire_u16 length,
					      tbv_wire_u16 flags,
					      tbv_wire_u32 seq)
{
	tbv_wire_put_le32(p, TBV_NATIVE_WIRE_MAGIC);
	tbv_wire_put_le16(p + 4, TBV_NATIVE_WIRE_VERSION);
	tbv_wire_put_le16(p + 6, op);
	tbv_wire_put_le16(p + 8, length);
	tbv_wire_put_le16(p + 10, flags);
	tbv_wire_put_le32(p + 12, seq);
}

static inline void tbv_native_wire_put_xdomain_header(tbv_wire_u8 *p,
						     tbv_wire_u64 route,
						     tbv_wire_u8 sequence,
						     tbv_wire_u32 type,
						     tbv_wire_u16 size)
{
	tbv_wire_u32 length_sn = (size - 12u) / 4u;

	length_sn |= ((tbv_wire_u32)sequence & 0x3u) << 27;

	tbv_wire_put_le32(p, route >> 32);
	tbv_wire_put_le32(p + 4, route & 0xffffffffull);
	tbv_wire_put_le32(p + 8, length_sn);
	memcpy(p + 12, tbv_native_wire_uuid, sizeof(tbv_native_wire_uuid));
	tbv_wire_put_le32(p + 28, type);
}

static inline int
tbv_native_wire_build_hello(void *buf, size_t size,
			    const struct tbv_native_wire_hello *hello,
			    tbv_wire_u16 op, tbv_wire_u16 flags,
			    tbv_wire_u32 seq, tbv_wire_u64 route,
			    tbv_wire_u8 xdomain_sequence)
{
	tbv_wire_u8 *p = buf;

	if (!p || !hello)
		return -EINVAL;

	if (op != TBV_NATIVE_WIRE_OP_HELLO &&
	    op != TBV_NATIVE_WIRE_OP_HELLO_ACK &&
	    op != TBV_NATIVE_WIRE_OP_READY &&
	    op != TBV_NATIVE_WIRE_OP_READY_ACK)
		return -EINVAL;

	if (size < TBV_NATIVE_WIRE_HELLO_MSG_SIZE)
		return -ENOSPC;

	memset(p, 0, TBV_NATIVE_WIRE_HELLO_MSG_SIZE);
	tbv_native_wire_put_xdomain_header(p, route, xdomain_sequence, op,
					   TBV_NATIVE_WIRE_HELLO_MSG_SIZE);
	p += TBV_NATIVE_WIRE_XDOMAIN_HDR_SIZE;
	tbv_native_wire_put_header(p, op, TBV_NATIVE_WIRE_HELLO_MSG_SIZE,
				   flags, seq);

	p += TBV_NATIVE_WIRE_HDR_SIZE;
	tbv_wire_put_le32(p, hello->capabilities);
	tbv_wire_put_le32(p + 4, hello->rail_id);
	tbv_wire_put_le64(p + 8, hello->route);
	tbv_wire_put_le32(p + 16, hello->tx_hop);
	tbv_wire_put_le32(p + 20, hello->rx_hop);
	tbv_wire_put_le32(p + 24, hello->transmit_path);
	tbv_wire_put_le32(p + 28, hello->tx_ring_size);
	tbv_wire_put_le32(p + 32, hello->rx_ring_size);
	tbv_wire_put_le32(p + 36, hello->path_flags);
	tbv_wire_put_le64(p + 40, hello->roce_eui64);
	tbv_wire_put_le32(p + 48, hello->roce_ipv4);
	tbv_wire_put_le32(p + 52, hello->reserved);

	return TBV_NATIVE_WIRE_HELLO_MSG_SIZE;
}

static inline int
tbv_native_wire_parse_hello(const void *buf, size_t size,
			    struct tbv_native_wire_hello *hello,
			    struct tbv_native_wire_info *info)
{
	const tbv_wire_u8 *p = buf;
	tbv_wire_u16 op;
	tbv_wire_u16 length;
	tbv_wire_u32 length_sn;

	if (!p || !hello)
		return -EINVAL;

	if (size < TBV_NATIVE_WIRE_HELLO_MSG_SIZE)
		return -EINVAL;

	length_sn = tbv_wire_get_le32(p + 8);
	if ((length_sn & 0x3fu) != (TBV_NATIVE_WIRE_HELLO_MSG_SIZE - 12u) / 4u)
		return -EINVAL;

	if (memcmp(p + 12, tbv_native_wire_uuid,
		   sizeof(tbv_native_wire_uuid)) != 0)
		return -EINVAL;

	if (tbv_wire_get_le32(p + 28) != TBV_NATIVE_WIRE_OP_HELLO &&
	    tbv_wire_get_le32(p + 28) != TBV_NATIVE_WIRE_OP_HELLO_ACK &&
	    tbv_wire_get_le32(p + 28) != TBV_NATIVE_WIRE_OP_READY &&
	    tbv_wire_get_le32(p + 28) != TBV_NATIVE_WIRE_OP_READY_ACK)
		return -EINVAL;

	if (info) {
		info->xdomain_sequence = (length_sn >> 27) & 0x3u;
		info->route = ((tbv_wire_u64)tbv_wire_get_le32(p) << 32) |
			      tbv_wire_get_le32(p + 4);
		info->route &= ~(1ull << 63);
	}

	p += TBV_NATIVE_WIRE_XDOMAIN_HDR_SIZE;
	if (tbv_wire_get_le32(p) != TBV_NATIVE_WIRE_MAGIC)
		return -EINVAL;

	if (tbv_wire_get_le16(p + 4) != TBV_NATIVE_WIRE_VERSION)
		return -EINVAL;

	op = tbv_wire_get_le16(p + 6);
	if (op != TBV_NATIVE_WIRE_OP_HELLO &&
	    op != TBV_NATIVE_WIRE_OP_HELLO_ACK &&
	    op != TBV_NATIVE_WIRE_OP_READY &&
	    op != TBV_NATIVE_WIRE_OP_READY_ACK)
		return -EINVAL;

	if (op != tbv_wire_get_le32((const tbv_wire_u8 *)buf + 28))
		return -EINVAL;

	length = tbv_wire_get_le16(p + 8);
	if (length != TBV_NATIVE_WIRE_HELLO_MSG_SIZE || size < length)
		return -EINVAL;

	if (info) {
		info->op = op;
		info->flags = tbv_wire_get_le16(p + 10);
		info->seq = tbv_wire_get_le32(p + 12);
	}

	p += TBV_NATIVE_WIRE_HDR_SIZE;
	hello->capabilities = tbv_wire_get_le32(p);
	hello->rail_id = tbv_wire_get_le32(p + 4);
	hello->route = tbv_wire_get_le64(p + 8);
	hello->tx_hop = tbv_wire_get_le32(p + 16);
	hello->rx_hop = tbv_wire_get_le32(p + 20);
	hello->transmit_path = tbv_wire_get_le32(p + 24);
	hello->tx_ring_size = tbv_wire_get_le32(p + 28);
	hello->rx_ring_size = tbv_wire_get_le32(p + 32);
	hello->path_flags = tbv_wire_get_le32(p + 36);
	hello->roce_eui64 = tbv_wire_get_le64(p + 40);
	hello->roce_ipv4 = tbv_wire_get_le32(p + 48);
	hello->reserved = tbv_wire_get_le32(p + 52);

	return 0;
}

/*
 * Best-effort wire-version peek for diagnostics. Returns the version field if
 * the buffer is a plausible native-wire message (magic matches), else -1.
 * Used to emit a loud dmesg warn when a peer speaks a different wire version
 * (mixed-version fleet during a rollout), since parse just returns -EINVAL
 * and the rail would otherwise time out silently.
 */
static inline int tbv_native_wire_peek_version(const void *buf, size_t size)
{
	const tbv_wire_u8 *p = buf;

	if (!p || size < TBV_NATIVE_WIRE_XDOMAIN_HDR_SIZE + 8u)
		return -1;
	if (memcmp(p + 12, tbv_native_wire_uuid,
		   sizeof(tbv_native_wire_uuid)) != 0)
		return -1;
	p += TBV_NATIVE_WIRE_XDOMAIN_HDR_SIZE;
	if (tbv_wire_get_le32(p) != TBV_NATIVE_WIRE_MAGIC)
		return -1;
	return tbv_wire_get_le16(p + 4);
}

#endif
