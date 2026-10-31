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

void tbrxe_bad_frame_diagnose(
		const void *data, u16 len,
		struct tbrxe_bad_frame_diagnostic *diagnostic)
{
	struct rxe_pkt_info pkt = {
		.hdr = (void *)data,
		.paylen = len,
	};
	u16 header_len;

	memset(diagnostic, 0, sizeof(*diagnostic));
	diagnostic->len = len;
	if (data && len)
		diagnostic->fingerprint = crc32_le(~0U, data, len);
	if (!data || len < RXE_BTH_BYTES) {
		diagnostic->icrc = TBRXE_BAD_FRAME_TOO_SHORT;
		return;
	}

	pkt.opcode = bth_opcode(&pkt);
	pkt.mask = rxe_opcode[pkt.opcode].mask;
	header_len = rxe_opcode[pkt.opcode].length;
	diagnostic->opcode = pkt.opcode;
	diagnostic->qpn = bth_qpn(&pkt);
	diagnostic->psn = bth_psn(&pkt);
	diagnostic->pad = bth_pad(&pkt);
	diagnostic->header_len = header_len;

	if (header_len < RXE_BTH_BYTES) {
		diagnostic->icrc = TBRXE_BAD_FRAME_UNSUPPORTED_OPCODE;
		return;
	}
	if (len < header_len + RXE_ICRC_SIZE + diagnostic->pad) {
		diagnostic->icrc = TBRXE_BAD_FRAME_TOO_SHORT;
		return;
	}

	diagnostic->icrc = rxe_icrc_check(NULL, &pkt) ?
		TBRXE_BAD_FRAME_ICRC_MISMATCH : TBRXE_BAD_FRAME_ICRC_MATCH;
}

#if IS_ENABLED(CONFIG_KUNIT)
#include <kunit/test.h>

static void tbrxe_bad_frame_diagnostic_recomputes_icrc(struct kunit *test)
{
	u8 frame[64] = {};
	struct rxe_pkt_info pkt = {
		.hdr = frame,
		.opcode = IB_OPCODE_RC_ACKNOWLEDGE,
		.mask = rxe_opcode[IB_OPCODE_RC_ACKNOWLEDGE].mask,
		.paylen = rxe_opcode[IB_OPCODE_RC_ACKNOWLEDGE].length +
			 RXE_ICRC_SIZE,
	};
	struct tbrxe_bad_frame_diagnostic diagnostic;

	bth_init(&pkt, pkt.opcode, 0, 0, 0, IB_DEFAULT_PKEY_FULL,
		 0x12345, 0, 0x654321);
	aeth_set_syn(&pkt, AETH_ACK_UNLIMITED);
	aeth_set_msn(&pkt, 7);
	rxe_icrc_generate(NULL, &pkt);

	tbrxe_bad_frame_diagnose(frame, pkt.paylen, &diagnostic);
	KUNIT_EXPECT_EQ(test, (int)TBRXE_BAD_FRAME_ICRC_MATCH,
			diagnostic.icrc);
	KUNIT_EXPECT_EQ(test, (u8)IB_OPCODE_RC_ACKNOWLEDGE,
			diagnostic.opcode);
	KUNIT_EXPECT_EQ(test, (u32)0x12345, diagnostic.qpn);
	KUNIT_EXPECT_EQ(test, (u32)0x654321, diagnostic.psn);
	KUNIT_EXPECT_EQ(test, (u16)pkt.paylen, diagnostic.len);

	frame[RXE_BTH_BYTES] ^= BIT(0);
	tbrxe_bad_frame_diagnose(frame, pkt.paylen, &diagnostic);
	KUNIT_EXPECT_EQ(test, (int)TBRXE_BAD_FRAME_ICRC_MISMATCH,
			diagnostic.icrc);
}

static void tbrxe_bad_frame_diagnostic_bounds_short_input(struct kunit *test)
{
	u8 frame[RXE_BTH_BYTES] = {};
	struct tbrxe_bad_frame_diagnostic diagnostic;

	tbrxe_bad_frame_diagnose(frame, RXE_BTH_BYTES - 1, &diagnostic);
	KUNIT_EXPECT_EQ(test, (int)TBRXE_BAD_FRAME_TOO_SHORT,
			diagnostic.icrc);

	bth_init(&(struct rxe_pkt_info) {
			.hdr = frame,
			.opcode = IB_OPCODE_RC_ACKNOWLEDGE,
		}, IB_OPCODE_RC_ACKNOWLEDGE, 0, 0, 0,
		IB_DEFAULT_PKEY_FULL, 1, 0, 2);
	tbrxe_bad_frame_diagnose(frame, RXE_BTH_BYTES, &diagnostic);
	KUNIT_EXPECT_EQ(test, (int)TBRXE_BAD_FRAME_TOO_SHORT,
			diagnostic.icrc);
}

static struct kunit_case tbrxe_bad_frame_diagnostic_cases[] = {
	KUNIT_CASE(tbrxe_bad_frame_diagnostic_recomputes_icrc),
	KUNIT_CASE(tbrxe_bad_frame_diagnostic_bounds_short_input),
	{}
};

static struct kunit_suite tbrxe_bad_frame_diagnostic_suite = {
	.name = "tbrxe_bad_frame_diagnostic",
	.test_cases = tbrxe_bad_frame_diagnostic_cases,
};

kunit_test_suite(tbrxe_bad_frame_diagnostic_suite);
#endif
