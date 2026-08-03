// SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB
/*
 * Copyright (c) 2016 Mellanox Technologies Ltd. All rights reserved.
 * Copyright (c) 2015 System Fabric Works, Inc. All rights reserved.
 */

#include <linux/crc32.h>

#include "rxe.h"
#include "rxe_loc.h"

/**
 * rxe_crc32() - Compute cumulative crc32 for a contiguous segment
 * @rxe: rdma_rxe device object
 * @crc: starting crc32 value from previous segments
 * @next: starting address of current segment
 * @len: length of current segment
 *
 * Return: the cumulative crc32 checksum
 */
static __be32 rxe_crc32(struct rxe_dev *rxe, __be32 crc, void *next, size_t len)
{
	return (__force __be32)crc32_le((__force u32)crc, next, len);
}

/**
 * rxe_icrc_hdr() - Compute the partial ICRC for the transport headers of a
 *		  packet.
 * @skb: packet buffer (unused; kept so the call sites stay verbatim)
 * @pkt: packet information
 *
 * tbrxe deviation from upstream rxe, per tbframe-tbrxe-wire-spec.md
 * section 4: the IP/UDP pseudo-header is removed entirely. Nothing mutates
 * a frame in flight on a single-hop Thunderbolt link, so the masked
 * pseudo-header existed only to tolerate router mutation. The seed and the
 * masked-BTH coverage are unchanged.
 *
 * Return: the partial ICRC
 */
static __be32 rxe_icrc_hdr(struct sk_buff *skb, struct rxe_pkt_info *pkt)
{
	u8 pshdr[RXE_BTH_BYTES];
	struct rxe_bth *bth;
	__be32 crc;

	/* This seed is the result of computing a CRC with a seed of
	 * 0xffffffff and 8 bytes of 0xff representing a masked LRH.
	 */
	crc = (__force __be32)0xdebb20e3;

	memcpy(pshdr, pkt->hdr, RXE_BTH_BYTES);
	bth = (struct rxe_bth *)pshdr;

	/* exclude bth.resv8a */
	bth->qpn |= cpu_to_be32(~BTH_QPN_MASK);

	crc = rxe_crc32(pkt->rxe, crc, pshdr, RXE_BTH_BYTES);

	/* And finish to compute the CRC on the remainder of the headers. */
	crc = rxe_crc32(pkt->rxe, crc, pkt->hdr + RXE_BTH_BYTES,
			rxe_opcode[pkt->opcode].length - RXE_BTH_BYTES);
	return crc;
}

/**
 * rxe_icrc_check() - Compute ICRC for a packet and compare to the ICRC
 *		      delivered in the packet.
 * @skb: packet buffer
 * @pkt: packet information
 *
 * Return: 0 if the values match else an error
 */
int rxe_icrc_check(struct sk_buff *skb, struct rxe_pkt_info *pkt)
{
	__be32 *icrcp;
	__be32 pkt_icrc;
	__be32 icrc;

	icrcp = (__be32 *)(pkt->hdr + pkt->paylen - RXE_ICRC_SIZE);
	pkt_icrc = *icrcp;

	icrc = rxe_icrc_hdr(skb, pkt);
	icrc = rxe_crc32(pkt->rxe, icrc, (u8 *)payload_addr(pkt),
				payload_size(pkt) + bth_pad(pkt));
	icrc = ~icrc;

	if (unlikely(icrc != pkt_icrc))
		return -EINVAL;

	return 0;
}

/**
 * rxe_icrc_generate() - compute ICRC for a packet.
 * @skb: packet buffer
 * @pkt: packet information
 */
void rxe_icrc_generate(struct sk_buff *skb, struct rxe_pkt_info *pkt)
{
	__be32 *icrcp;
	__be32 icrc;

	icrcp = (__be32 *)(pkt->hdr + pkt->paylen - RXE_ICRC_SIZE);
	icrc = rxe_icrc_hdr(skb, pkt);
	icrc = rxe_crc32(pkt->rxe, icrc, (u8 *)payload_addr(pkt),
				payload_size(pkt) + bth_pad(pkt));
	*icrcp = ~icrc;
}
