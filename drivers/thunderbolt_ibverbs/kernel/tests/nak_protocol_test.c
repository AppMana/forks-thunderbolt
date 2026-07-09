// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit: receiver NAK + sender selective retransmit.
 *
 * Hardware (12-node TB chain, data_rx_bad_header 0.1-0.5% of messages): a
 * receiver that sees a head-of-line gap today buffers or drops the later
 * fragments and sends NOTHING -- the sender waits its full retransmit
 * deadline (>= 10 ms) and then re-copies and re-posts ALL ~330 frames. One
 * lost frame measurably stalls ~8 pipelined messages.
 *
 * The fix: the receiver names the hole immediately with a
 * TBV_NATIVE_DATA_OP_NAK control frame (psn + first missing byte + missing
 * length), and the sender retransmits only the fragments covering it,
 * treating the NAK's frag_offset as a cumulative SACK for later timeout
 * retransmits. Negotiated via TBV_NATIVE_WIRE_CAP_NAK: to an old peer no NAK
 * is ever emitted and behavior stays timer-driven whole-message retransmit.
 *
 * These tests pin:
 *   (a) the NAK wire format round-trips and its reserved fields are
 *       enforced;
 *   (b) emission gating: no cap => never sent; duplicate (psn, hole) NAKs
 *       are suppressed until the hole moves or the re-arm interval passes;
 *   (c) the sender's retransmit range: plain WRITE resends only the hole
 *       (the receiver buffers past it and merges), SEND/WRITE_IMM go back
 *       to the end (their receive paths cannot buffer past a hole), a
 *       whole-message NAK (0,0) resends everything, and the cumulative SACK
 *       floor drops known-delivered fragments from timeout retransmits;
 *   (d) a loss-recovery simulation: with NAK the sender resends exactly the
 *       missing window; without the cap the receiver stays silent and the
 *       timer path resends the whole message -- today's behavior preserved.
 *
 * Built into the module on CONFIG_KUNIT; run via tools/run-kunit.sh.
 */
#include <kunit/test.h>
#include <linux/jiffies.h>
#include "../../proto/native_data.h"
#include "../../proto/native_wire.h"
#include "../tbv.h"

static void tbv_nak_wire_format(struct kunit *test)
{
	struct tbv_native_data_header hdr = {};
	struct tbv_native_data_header parsed;
	u8 frame[TBV_NATIVE_DATA_HDR_SIZE];

	hdr.opcode = TBV_NATIVE_DATA_OP_NAK;
	hdr.dest_qp = 0x901;
	hdr.src_qp = 0x902;
	hdr.psn = 77;
	hdr.frag_offset = 5u * 4096u;	/* first missing byte */
	hdr.imm_data = 4096u;		/* missing length */

	KUNIT_ASSERT_EQ(test,
		tbv_native_data_build_header(frame, sizeof(frame), &hdr),
		(int)TBV_NATIVE_DATA_HDR_SIZE);
	KUNIT_ASSERT_EQ(test,
		tbv_native_data_parse_header(frame, sizeof(frame), &parsed), 0);
	KUNIT_EXPECT_EQ(test, parsed.opcode, TBV_NATIVE_DATA_OP_NAK);
	KUNIT_EXPECT_EQ(test, parsed.psn, 77u);
	KUNIT_EXPECT_EQ(test, parsed.frag_offset, 5u * 4096u);
	KUNIT_EXPECT_EQ(test, parsed.imm_data, 4096u);
	KUNIT_EXPECT_TRUE(test, tbv_native_data_valid_nak(&parsed));

	/* reserved fields must be zero */
	parsed.length = 1;
	KUNIT_EXPECT_FALSE(test, tbv_native_data_valid_nak(&parsed));
	parsed.length = 0;
	parsed.rkey = 1;
	KUNIT_EXPECT_FALSE(test, tbv_native_data_valid_nak(&parsed));
	parsed.rkey = 0;
	parsed.flags = TBV_NATIVE_DATA_F_LAST;
	KUNIT_EXPECT_FALSE(test, tbv_native_data_valid_nak(&parsed));
}

static void tbv_nak_emission_gating(struct kunit *test)
{
	unsigned long now = jiffies;
	unsigned long interval = msecs_to_jiffies(2);

	/* peer without the cap: NEVER send (old behavior preserved) */
	KUNIT_EXPECT_FALSE(test,
		tbv_rx_nak_should_send(TBV_NATIVE_WIRE_CAP_RC, false, 0, 0, 0,
				       7, 4096, now, interval));
	/* first gap on a capable peer: send */
	KUNIT_EXPECT_TRUE(test,
		tbv_rx_nak_should_send(TBV_NATIVE_WIRE_CAP_NAK, false, 0, 0, 0,
				       7, 4096, now, interval));
	/* same (psn, hole start) again immediately: suppressed */
	KUNIT_EXPECT_FALSE(test,
		tbv_rx_nak_should_send(TBV_NATIVE_WIRE_CAP_NAK, true, 7, 4096,
				       now, 7, 4096, now, interval));
	/* the hole moved (earlier bytes arrived): re-arm */
	KUNIT_EXPECT_TRUE(test,
		tbv_rx_nak_should_send(TBV_NATIVE_WIRE_CAP_NAK, true, 7, 4096,
				       now, 7, 8192, now, interval));
	/* a different message: re-arm */
	KUNIT_EXPECT_TRUE(test,
		tbv_rx_nak_should_send(TBV_NATIVE_WIRE_CAP_NAK, true, 7, 4096,
				       now, 8, 4096, now, interval));
	/* same hole but the re-arm interval passed (lost NAK): send again */
	KUNIT_EXPECT_TRUE(test,
		tbv_rx_nak_should_send(TBV_NATIVE_WIRE_CAP_NAK, true, 7, 4096,
				       now, 7, 4096, now + interval + 1,
				       interval));
}

static void tbv_nak_sender_retry_range(struct kunit *test)
{
	u32 total = 330u * 4096u;
	u32 start;
	u32 end;

	/* plain WRITE: the receiver buffers past the hole, resend it alone */
	tbv_send_retry_range(TBV_NATIVE_DATA_OP_RDMA_WRITE, total, 0, true,
			     5u * 4096u, 4096u, &start, &end);
	KUNIT_EXPECT_EQ(test, start, 5u * 4096u);
	KUNIT_EXPECT_EQ(test, end, 6u * 4096u);

	/* missing length 0 = through the end of the message */
	tbv_send_retry_range(TBV_NATIVE_DATA_OP_RDMA_WRITE, total, 0, true,
			     5u * 4096u, 0, &start, &end);
	KUNIT_EXPECT_EQ(test, start, 5u * 4096u);
	KUNIT_EXPECT_EQ(test, end, total);

	/* WRITE_IMM / SEND receive paths drop past a hole: go-back-N */
	tbv_send_retry_range(TBV_NATIVE_DATA_OP_RDMA_WRITE_IMM, total, 0, true,
			     5u * 4096u, 4096u, &start, &end);
	KUNIT_EXPECT_EQ(test, start, 5u * 4096u);
	KUNIT_EXPECT_EQ(test, end, total);
	tbv_send_retry_range(TBV_NATIVE_DATA_OP_SEND, total, 0, true,
			     5u * 4096u, 4096u, &start, &end);
	KUNIT_EXPECT_EQ(test, end, total);

	/* whole-message NAK (psn gap): resend everything */
	tbv_send_retry_range(TBV_NATIVE_DATA_OP_RDMA_WRITE, total, 0, true,
			     0, 0, &start, &end);
	KUNIT_EXPECT_EQ(test, start, 0u);
	KUNIT_EXPECT_EQ(test, end, total);

	/* timeout retransmit honors the cumulative SACK floor */
	tbv_send_retry_range(TBV_NATIVE_DATA_OP_RDMA_WRITE, total,
			     100u * 4096u, false, 0, 0, &start, &end);
	KUNIT_EXPECT_EQ(test, start, 100u * 4096u);
	KUNIT_EXPECT_EQ(test, end, total);

	/* the SACK floor also clips a NAK range from a stale hole */
	tbv_send_retry_range(TBV_NATIVE_DATA_OP_RDMA_WRITE, total,
			     6u * 4096u, true, 5u * 4096u, 8192u, &start, &end);
	KUNIT_EXPECT_EQ(test, start, 6u * 4096u);
	KUNIT_EXPECT_EQ(test, end, 7u * 4096u);
}

static void tbv_nak_sack_and_frag_filter(struct kunit *test)
{
	/* monotone, clamped cumulative SACK */
	KUNIT_EXPECT_EQ(test, tbv_send_acked_prefix_update(0, 8192, SZ_1M),
			8192u);
	KUNIT_EXPECT_EQ(test, tbv_send_acked_prefix_update(8192, 4096, SZ_1M),
			8192u);
	KUNIT_EXPECT_EQ(test, tbv_send_acked_prefix_update(0, SZ_2M, SZ_1M),
			(u32)SZ_1M);

	/* fragment intersection with the retry range */
	KUNIT_EXPECT_TRUE(test, tbv_send_frag_needed(4096, 4096, 4096, 8192));
	KUNIT_EXPECT_TRUE(test, tbv_send_frag_needed(0, 8192, 4096, 8192));
	KUNIT_EXPECT_FALSE(test, tbv_send_frag_needed(0, 4096, 4096, 8192));
	KUNIT_EXPECT_FALSE(test, tbv_send_frag_needed(8192, 4096, 4096, 8192));
	/* zero-length message: its single frame is always needed */
	KUNIT_EXPECT_TRUE(test, tbv_send_frag_needed(0, 0, 0, 0));
}

/*
 * Loss-recovery simulation over the pure helpers: 8 windows, window 2 lost.
 * Receiver watermark stops at the hole; the arriving window 3 names it. With
 * the cap the sender resends exactly window 2; without it nothing is sent
 * until the timer fires and the whole message repeats.
 */
static void tbv_nak_recovery_simulation(struct kunit *test)
{
	u32 unit = TBV_NATIVE_DATA_SPLIT_UNIT;
	u32 total = 8u * unit;
	u32 received = 2u * unit;	/* watermark: windows 0,1 delivered */
	u32 arriving_off = 3u * unit;	/* window 3 arrives -> gap */
	unsigned long now = jiffies;
	u32 resent = 0;
	u32 start;
	u32 end;
	u32 idx;

	/* capable peer: NAK names [received, arriving_off) */
	KUNIT_ASSERT_TRUE(test,
		tbv_rx_nak_should_send(TBV_NATIVE_WIRE_CAP_NAK, false, 0, 0, 0,
				       9, received, now, msecs_to_jiffies(2)));
	tbv_send_retry_range(TBV_NATIVE_DATA_OP_RDMA_WRITE, total, 0, true,
			     received, arriving_off - received, &start, &end);
	for (idx = 0; idx < tbv_native_data_split_window_count(total); idx++) {
		u32 off;
		u32 len;

		KUNIT_ASSERT_EQ(test,
			tbv_native_data_split_window(total, idx, &off, &len),
			0);
		if (tbv_send_frag_needed(off, len, start, end))
			resent++;
	}
	KUNIT_EXPECT_EQ(test, resent, 1u);

	/* old peer: silent; the timeout path later resends all 8 windows */
	KUNIT_EXPECT_FALSE(test,
		tbv_rx_nak_should_send(TBV_NATIVE_WIRE_CAP_RC, false, 0, 0, 0,
				       9, received, now, msecs_to_jiffies(2)));
	tbv_send_retry_range(TBV_NATIVE_DATA_OP_RDMA_WRITE, total, 0, false,
			     0, 0, &start, &end);
	resent = 0;
	for (idx = 0; idx < tbv_native_data_split_window_count(total); idx++) {
		u32 off;
		u32 len;

		KUNIT_ASSERT_EQ(test,
			tbv_native_data_split_window(total, idx, &off, &len),
			0);
		if (tbv_send_frag_needed(off, len, start, end))
			resent++;
	}
	KUNIT_EXPECT_EQ(test, resent, 8u);
}

static struct kunit_case tbv_nak_protocol_cases[] = {
	KUNIT_CASE(tbv_nak_wire_format),
	KUNIT_CASE(tbv_nak_emission_gating),
	KUNIT_CASE(tbv_nak_sender_retry_range),
	KUNIT_CASE(tbv_nak_sack_and_frag_filter),
	KUNIT_CASE(tbv_nak_recovery_simulation),
	{}
};

static struct kunit_suite tbv_nak_protocol_suite = {
	.name = "thunderbolt_ibverbs_nak_protocol",
	.test_cases = tbv_nak_protocol_cases,
};
kunit_test_suite(tbv_nak_protocol_suite);
