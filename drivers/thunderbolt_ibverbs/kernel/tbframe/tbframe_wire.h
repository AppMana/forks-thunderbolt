/* SPDX-License-Identifier: GPL-2.0 */
/*
 * tbframe control-plane wire format (spec docs/tbframe-tbrxe-wire-spec.md §3).
 *
 * HELLO/READY ride the XDomain control channel (ring 0) exactly like
 * ThunderboltIP login and the legacy tbverbs HELLO; the serialization
 * mechanics mirror proto/native_wire.h so both protocols coexist on one
 * link during the transition (distinct UUIDs demultiplex them).
 */
#ifndef TBFRAME_WIRE_H
#define TBFRAME_WIRE_H

#include <linux/errno.h>
#include <linux/string.h>
#include <linux/types.h>

#define TBFRAME_WIRE_MAGIC		0x31464254u /* "TBF1" little-endian */
/* v2: distinct SOF marker PDF (0x6) on the data path; v1 peers chop
 * multi-packet frames, so mixed versions must refuse the session.
 */
#define TBFRAME_WIRE_VERSION		2u
#define TBFRAME_WIRE_XDOMAIN_HDR_SIZE	32u
#define TBFRAME_WIRE_HDR_SIZE		16u
#define TBFRAME_WIRE_HELLO_SIZE		28u
#define TBFRAME_WIRE_HELLO_MSG_SIZE \
	(TBFRAME_WIRE_XDOMAIN_HDR_SIZE + TBFRAME_WIRE_HDR_SIZE + \
	 TBFRAME_WIRE_HELLO_SIZE)

static const u8 tbframe_wire_uuid[16] = {
	0x9a, 0x52, 0xc3, 0xb4, 0x1f, 0x6e, 0x4d, 0x07,
	0x8b, 0x2a, 0x46, 0xc8, 0x1d, 0x90, 0xe5, 0xf3,
};

enum tbframe_wire_op {
	TBFRAME_WIRE_OP_HELLO = 1,
	TBFRAME_WIRE_OP_HELLO_ACK = 2,
	TBFRAME_WIRE_OP_READY = 3,
	TBFRAME_WIRE_OP_READY_ACK = 4,
};

/* HELLO capability word (spec §3): additive, unknown bits ignored. */
#define TBFRAME_WIRE_CAP_E2E		BIT(0)
#define TBFRAME_WIRE_CAP_KEEPALIVE	BIT(1)

struct tbframe_wire_hello {
	u16	proto_version;
	u16	transmit_hopid;
	u16	rx_ring_entries;
	u32	capabilities;
	u64	gid_eui64;
	u64	session_cookie;
};

struct tbframe_wire_info {
	u16	op;
	u32	seq;
	u8	xdomain_sequence;
	u64	route;
};

static inline void tbframe_wire_put_le16(u8 *p, u16 value)
{
	p[0] = value & 0xffu;
	p[1] = value >> 8;
}

static inline void tbframe_wire_put_le32(u8 *p, u32 value)
{
	p[0] = value & 0xffu;
	p[1] = (value >> 8) & 0xffu;
	p[2] = (value >> 16) & 0xffu;
	p[3] = value >> 24;
}

static inline void tbframe_wire_put_le64(u8 *p, u64 value)
{
	tbframe_wire_put_le32(p, value & 0xffffffffull);
	tbframe_wire_put_le32(p + 4, value >> 32);
}

static inline u16 tbframe_wire_get_le16(const u8 *p)
{
	return (u16)p[0] | ((u16)p[1] << 8);
}

static inline u32 tbframe_wire_get_le32(const u8 *p)
{
	return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) |
	       ((u32)p[3] << 24);
}

static inline u64 tbframe_wire_get_le64(const u8 *p)
{
	return (u64)tbframe_wire_get_le32(p) |
	       ((u64)tbframe_wire_get_le32(p + 4) << 32);
}

static inline int tbframe_wire_build_hello(void *buf, size_t size,
					   const struct tbframe_wire_hello *hello,
					   u16 op, u32 seq, u64 route,
					   u8 xdomain_sequence)
{
	u8 *p = buf;
	u32 length_sn;

	if (!p || !hello)
		return -EINVAL;
	if (op < TBFRAME_WIRE_OP_HELLO || op > TBFRAME_WIRE_OP_READY_ACK)
		return -EINVAL;
	if (size < TBFRAME_WIRE_HELLO_MSG_SIZE)
		return -ENOSPC;

	memset(p, 0, TBFRAME_WIRE_HELLO_MSG_SIZE);

	/* XDomain header: route, length/SN word, protocol UUID, packet type. */
	length_sn = (TBFRAME_WIRE_HELLO_MSG_SIZE - 12u) / 4u;
	length_sn |= ((u32)xdomain_sequence & 0x3u) << 27;
	tbframe_wire_put_le32(p, route >> 32);
	tbframe_wire_put_le32(p + 4, route & 0xffffffffull);
	tbframe_wire_put_le32(p + 8, length_sn);
	memcpy(p + 12, tbframe_wire_uuid, sizeof(tbframe_wire_uuid));
	tbframe_wire_put_le32(p + 28, op);

	p += TBFRAME_WIRE_XDOMAIN_HDR_SIZE;
	tbframe_wire_put_le32(p, TBFRAME_WIRE_MAGIC);
	tbframe_wire_put_le16(p + 4, TBFRAME_WIRE_VERSION);
	tbframe_wire_put_le16(p + 6, op);
	tbframe_wire_put_le16(p + 8, TBFRAME_WIRE_HELLO_MSG_SIZE);
	tbframe_wire_put_le16(p + 10, 0);
	tbframe_wire_put_le32(p + 12, seq);

	p += TBFRAME_WIRE_HDR_SIZE;
	tbframe_wire_put_le16(p, hello->proto_version);
	tbframe_wire_put_le16(p + 2, hello->transmit_hopid);
	tbframe_wire_put_le16(p + 4, hello->rx_ring_entries);
	tbframe_wire_put_le16(p + 6, 0);
	tbframe_wire_put_le32(p + 8, hello->capabilities);
	tbframe_wire_put_le64(p + 12, hello->gid_eui64);
	tbframe_wire_put_le64(p + 20, hello->session_cookie);

	return TBFRAME_WIRE_HELLO_MSG_SIZE;
}

static inline int tbframe_wire_parse_hello(const void *buf, size_t size,
					   struct tbframe_wire_hello *hello,
					   struct tbframe_wire_info *info)
{
	const u8 *p = buf;
	u32 length_sn;
	u16 op;

	if (!p || !hello)
		return -EINVAL;
	if (size < TBFRAME_WIRE_HELLO_MSG_SIZE)
		return -EINVAL;

	length_sn = tbframe_wire_get_le32(p + 8);
	if ((length_sn & 0x3fu) != (TBFRAME_WIRE_HELLO_MSG_SIZE - 12u) / 4u)
		return -EINVAL;
	if (memcmp(p + 12, tbframe_wire_uuid, sizeof(tbframe_wire_uuid)))
		return -EINVAL;

	op = tbframe_wire_get_le32(p + 28);
	if (op < TBFRAME_WIRE_OP_HELLO || op > TBFRAME_WIRE_OP_READY_ACK)
		return -EINVAL;

	if (info) {
		info->xdomain_sequence = (length_sn >> 27) & 0x3u;
		info->route = ((u64)tbframe_wire_get_le32(p) << 32) |
			      tbframe_wire_get_le32(p + 4);
		info->route &= ~(1ull << 63);
	}

	p += TBFRAME_WIRE_XDOMAIN_HDR_SIZE;
	if (tbframe_wire_get_le32(p) != TBFRAME_WIRE_MAGIC)
		return -EINVAL;
	if (tbframe_wire_get_le16(p + 4) != TBFRAME_WIRE_VERSION)
		return -EINVAL;
	if (tbframe_wire_get_le16(p + 6) != op)
		return -EINVAL;
	if (tbframe_wire_get_le16(p + 8) != TBFRAME_WIRE_HELLO_MSG_SIZE)
		return -EINVAL;

	if (info) {
		info->op = op;
		info->seq = tbframe_wire_get_le32(p + 12);
	}

	p += TBFRAME_WIRE_HDR_SIZE;
	hello->proto_version = tbframe_wire_get_le16(p);
	hello->transmit_hopid = tbframe_wire_get_le16(p + 2);
	hello->rx_ring_entries = tbframe_wire_get_le16(p + 4);
	hello->capabilities = tbframe_wire_get_le32(p + 8);
	hello->gid_eui64 = tbframe_wire_get_le64(p + 12);
	hello->session_cookie = tbframe_wire_get_le64(p + 20);

	return 0;
}

/*
 * Best-effort version peek for the mixed-fleet dmesg warn (parse just
 * returns -EINVAL and the link would otherwise time out silently).
 */
static inline int tbframe_wire_peek_version(const void *buf, size_t size)
{
	const u8 *p = buf;

	if (!p || size < TBFRAME_WIRE_XDOMAIN_HDR_SIZE + 8u)
		return -1;
	if (memcmp(p + 12, tbframe_wire_uuid, sizeof(tbframe_wire_uuid)))
		return -1;
	p += TBFRAME_WIRE_XDOMAIN_HDR_SIZE;
	if (tbframe_wire_get_le32(p) != TBFRAME_WIRE_MAGIC)
		return -1;
	return tbframe_wire_get_le16(p + 4);
}

#endif /* TBFRAME_WIRE_H */
