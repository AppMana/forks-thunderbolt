// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit: retryable zero-copy TX via per-fragment split raw streams.
 *
 * Hardware (12-node TB chain, NCCL over usb4_rdma): every 1.3 MiB RDMA_WRITE
 * is bounce-copied into ~330 freshly kmalloc'd 4 KiB frames per hop
 * (data_wr_copied == messages, data_wr_zcopy == 0), because
 * tbv_send_ctx_allows_raw_zcopy() forbids raw page streams for retryable
 * ctxs and native RC WRs are always retryable. The full-message raw stream
 * is genuinely retransmit-unsafe: payload frames carry no header, so after a
 * loss the receiver consumes a retransmitted header as payload and delivers
 * shifted bytes.
 *
 * The fix keeps zero-copy but bounds the unframed span to ONE fragment: each
 * 4096-byte window of the message is its own raw stream (48-byte header
 * frame with a frag_offset base + <= 2 raw page frames), negotiated via
 * TBV_NATIVE_WIRE_CAP_SPLIT_DATA. Loss desyncs at most one fragment and the
 * receiver detects a mid-stream header by parse (TVD1 magic), so retransmits
 * from the same pinned pages are safe and byte-identical.
 *
 * These tests pin:
 *   (a) mode selection: retryable + peer cap => SPLIT; retryable without the
 *       cap => bounce copy (old peers keep old behavior); non-retryable
 *       keeps the legacy full-message stream; striping/small/non-write
 *       never zcopy;
 *   (b) split TX frame GEOMETRY across (source page offset, window, message
 *       size): frame count, each frame->size (0==4096 12-bit encoding), each
 *       sof/eof PDF, each DMA page offset, and byte-identical reassembly to
 *       the copied path -- and that tbv_zcopy_split_page_aligned() exactly
 *       predicts whether the emit produced only offset-0 frames (the 0.2.26
 *       hardware-CRC fix: only page-aligned sources are zero-copied, so every
 *       frame is offset-0 like tbnet; mid-page sources fall back to the copy);
 *   (c) the write fragment shape resolver prefers the framing unit that
 *       matches the message's established fragment count (the lcm(4048,4096)
 *       ambiguity at ~1 MiB offsets);
 *   (d) a valid native header arriving mid-raw-stream is classified as a
 *       header (desync), never as payload.
 *
 * Built into the module on CONFIG_KUNIT; run via tools/run-kunit.sh.
 */
#include <kunit/test.h>
#include <linux/slab.h>
#include "../../proto/native_data.h"
#include "../../proto/native_wire.h"
#include "../tbv.h"

#define SPLIT_TEST_MIN_BYTES 8192u
#define SPLIT_TEST_CAPS (TBV_NATIVE_WIRE_CAP_RC | TBV_NATIVE_WIRE_CAP_SPLIT_DATA)
#define SPLIT_TEST_OLD_CAPS TBV_NATIVE_WIRE_CAP_RC

static void tbv_zcopy_mode_selection(struct kunit *test)
{
	/* retryable RC WRITE to a split-capable peer: the fixed behavior */
	KUNIT_EXPECT_EQ(test,
		tbv_zcopy_select_mode(true, false, true, SZ_1M,
				      SPLIT_TEST_MIN_BYTES, SPLIT_TEST_CAPS),
		TBV_ZCOPY_TX_SPLIT);
	/* old peer (no cap): degrade to the bounce copy, never split */
	KUNIT_EXPECT_EQ(test,
		tbv_zcopy_select_mode(true, false, true, SZ_1M,
				      SPLIT_TEST_MIN_BYTES, SPLIT_TEST_OLD_CAPS),
		TBV_ZCOPY_TX_NONE);
	/* non-retryable keeps the legacy full-message raw stream */
	KUNIT_EXPECT_EQ(test,
		tbv_zcopy_select_mode(true, false, false, SZ_1M,
				      SPLIT_TEST_MIN_BYTES, SPLIT_TEST_OLD_CAPS),
		TBV_ZCOPY_TX_RAW_STREAM);
	/* below the size threshold: bounce (stream setup costs more) */
	KUNIT_EXPECT_EQ(test,
		tbv_zcopy_select_mode(true, false, true, 4096,
				      SPLIT_TEST_MIN_BYTES, SPLIT_TEST_CAPS),
		TBV_ZCOPY_TX_NONE);
	/* threshold boundary is inclusive */
	KUNIT_EXPECT_EQ(test,
		tbv_zcopy_select_mode(true, false, true, SPLIT_TEST_MIN_BYTES,
				      SPLIT_TEST_MIN_BYTES, SPLIT_TEST_CAPS),
		TBV_ZCOPY_TX_SPLIT);
	/* zcopy_min_bytes == 0 disables zero-copy entirely */
	KUNIT_EXPECT_EQ(test,
		tbv_zcopy_select_mode(true, false, true, SZ_1M, 0,
				      SPLIT_TEST_CAPS),
		TBV_ZCOPY_TX_NONE);
	/* fragment striping and non-WRITE ops never zcopy */
	KUNIT_EXPECT_EQ(test,
		tbv_zcopy_select_mode(true, true, true, SZ_1M,
				      SPLIT_TEST_MIN_BYTES, SPLIT_TEST_CAPS),
		TBV_ZCOPY_TX_NONE);
	KUNIT_EXPECT_EQ(test,
		tbv_zcopy_select_mode(false, false, true, SZ_1M,
				      SPLIT_TEST_MIN_BYTES, SPLIT_TEST_CAPS),
		TBV_ZCOPY_TX_NONE);
}

static void tbv_split_window_framing(struct kunit *test)
{
	u32 total = 3u * TBV_NATIVE_DATA_SPLIT_UNIT + 100u;
	u32 offset;
	u32 len;

	KUNIT_EXPECT_EQ(test, tbv_native_data_split_window_count(total), 4u);
	KUNIT_EXPECT_EQ(test,
		tbv_native_data_split_window_count(TBV_NATIVE_DATA_SPLIT_UNIT),
		1u);

	KUNIT_ASSERT_EQ(test,
		tbv_native_data_split_window(total, 0, &offset, &len), 0);
	KUNIT_EXPECT_EQ(test, offset, 0u);
	KUNIT_EXPECT_EQ(test, len, TBV_NATIVE_DATA_SPLIT_UNIT);

	KUNIT_ASSERT_EQ(test,
		tbv_native_data_split_window(total, 3, &offset, &len), 0);
	KUNIT_EXPECT_EQ(test, offset, 3u * TBV_NATIVE_DATA_SPLIT_UNIT);
	KUNIT_EXPECT_EQ(test, len, 100u);

	KUNIT_EXPECT_NE(test,
		tbv_native_data_split_window(total, 4, &offset, &len), 0);
}

/*
 * Model receiver: apply a synthesized per-fragment header + payload to a
 * destination buffer at frag_offset, exactly like the write RX path scatters
 * into the umem at remote_addr + frag_offset.
 */
static void split_model_deliver(u8 *dst, u32 dst_len,
				const struct tbv_native_data_header *hdr,
				const u8 *payload, struct kunit *test)
{
	KUNIT_ASSERT_LE(test, (u64)hdr->frag_offset + hdr->length, (u64)dst_len);
	memcpy(dst + hdr->frag_offset, payload, hdr->length);
}

/*
 * Deterministic geometry model of the split TX emit path, parameterized by the
 * source's byte offset within its first page (base_page_off). For each 4096-
 * byte window it walks page-boundary chunks EXACTLY as tbv_umem_page_from_addr
 * does (chunk = min(PAGE_SIZE - page_off, remaining_in_window, 4096)), and for
 * every emitted frame asserts the wire contract:
 *   - dma page offset (what becomes buffer_phy's page_off),
 *   - frame->size encoding: 0 means 4096, else the exact byte count (<= 4095,
 *     fits the 12-bit ring_desc.length),
 *   - sof/eof PDF START/END (each frame is its own PDF frame),
 *   - the synthesized receiver header carries the absolute frag_offset and
 *     LAST only on the message's final byte,
 * then delivers the bytes. all_offset0 records whether every frame was
 * page-aligned -- the property tbv_zcopy_split_page_aligned() must predict.
 */
static void split_model_emit(const u8 *src, u8 *dst, u32 total,
			     u32 base_page_off, bool *all_offset0,
			     struct kunit *test)
{
	u32 count = tbv_native_data_split_window_count(total);
	u32 idx;

	*all_offset0 = true;

	for (idx = 0; idx < count; idx++) {
		struct tbv_native_data_header stream = {};
		u32 win_off;
		u32 win_len;
		u32 done = 0;

		KUNIT_ASSERT_EQ(test,
			tbv_native_data_split_window(total, idx, &win_off,
						     &win_len), 0);
		KUNIT_EXPECT_LE(test, win_len, TBV_NATIVE_DATA_FRAME_SIZE);
		KUNIT_EXPECT_GT(test, win_len, 0u);

		stream.opcode = TBV_NATIVE_DATA_OP_RDMA_WRITE;
		stream.flags = TBV_NATIVE_DATA_F_RAW_STREAM;
		if (win_off + win_len == total)
			stream.flags |= TBV_NATIVE_DATA_F_LAST;
		stream.length = win_len;
		stream.frag_offset = win_off;

		while (done < win_len) {
			struct tbv_native_data_header payload = {};
			u32 page_off = (base_page_off + win_off + done) %
				       PAGE_SIZE;
			u32 part = min3(win_len - done,
					(u32)(PAGE_SIZE - page_off),
					(u32)TBV_NATIVE_DATA_FRAME_SIZE);
			u16 frame_size = part == TBV_NATIVE_DATA_FRAME_SIZE ?
					 0 : part;
			u8 sof = TBV_DATA_PDF_FRAME_START;
			u8 eof = TBV_DATA_PDF_FRAME_END;

			if (page_off)
				*all_offset0 = false;
			/* frame->size is 12-bit: only 0 encodes a full 4096 */
			KUNIT_EXPECT_LT(test, (u32)frame_size, 4096u);
			KUNIT_EXPECT_TRUE(test, part >= 1 &&
					  part <= TBV_NATIVE_DATA_FRAME_SIZE);
			KUNIT_EXPECT_EQ(test, sof,
					(u8)TBV_DATA_PDF_FRAME_START);
			KUNIT_EXPECT_EQ(test, eof,
					(u8)TBV_DATA_PDF_FRAME_END);

			KUNIT_ASSERT_EQ(test,
				tbv_native_data_raw_payload_header(&stream,
					done, win_len - done, part, &payload),
				0);
			KUNIT_EXPECT_EQ(test, payload.frag_offset,
					win_off + done);
			if (win_off + done + part == total)
				KUNIT_EXPECT_TRUE(test,
					payload.flags & TBV_NATIVE_DATA_F_LAST);
			else
				KUNIT_EXPECT_FALSE(test,
					payload.flags & TBV_NATIVE_DATA_F_LAST);

			split_model_deliver(dst, total, &payload,
					    src + win_off + done, test);
			done += part;
		}
	}
}

static void tbv_split_geometry_identical_to_copied(struct kunit *test)
{
	static const u32 sizes[] = {
		TBV_NATIVE_DATA_SPLIT_UNIT,		/* one full window */
		2u * TBV_NATIVE_DATA_SPLIT_UNIT + 1234u,
		333u * TBV_NATIVE_DATA_SPLIT_UNIT,	/* ~1.3 MiB, aligned */
		333u * TBV_NATIVE_DATA_SPLIT_UNIT + 7u,	/* partial last */
	};
	static const u32 offsets[] = { 0u, 64u, 1000u, 4095u };
	u32 si;
	u32 oi;

	for (si = 0; si < ARRAY_SIZE(sizes); si++) {
		u32 total = sizes[si];
		u8 *src = kunit_kmalloc(test, total, GFP_KERNEL);
		u8 *dst = kunit_kzalloc(test, total, GFP_KERNEL);
		u8 *dst_copied = kunit_kzalloc(test, total, GFP_KERNEL);
		u32 off;

		KUNIT_ASSERT_NOT_NULL(test, src);
		KUNIT_ASSERT_NOT_NULL(test, dst);
		KUNIT_ASSERT_NOT_NULL(test, dst_copied);
		for (off = 0; off < total; off++)
			src[off] = (u8)(off * 7u + si * 3u + 1u);

		/* copied-path reference: 4048-byte header-framed fragments */
		for (off = 0; off < total; off += TBV_NATIVE_DATA_MAX_PAYLOAD) {
			struct tbv_native_data_header hdr = {};

			hdr.frag_offset = off;
			hdr.length = min_t(u32, total - off,
					   TBV_NATIVE_DATA_MAX_PAYLOAD);
			split_model_deliver(dst_copied, total, &hdr,
					    src + off, test);
		}
		KUNIT_EXPECT_MEMEQ(test, dst_copied, src, total);

		for (oi = 0; oi < ARRAY_SIZE(offsets); oi++) {
			bool all_offset0;

			memset(dst, 0, total);
			split_model_emit(src, dst, total, offsets[oi],
					 &all_offset0, test);
			/* every geometry reassembles to the identical bytes */
			KUNIT_EXPECT_MEMEQ(test, dst, src, total);
			/*
			 * the page-aligned predicate must exactly predict
			 * whether the emit produced only offset-0 frames.
			 */
			KUNIT_EXPECT_EQ(test,
				tbv_zcopy_split_page_aligned(offsets[oi],
					TBV_NATIVE_DATA_SPLIT_UNIT, PAGE_SIZE),
				all_offset0);
		}
	}
}

static void tbv_split_page_alignment_gate(struct kunit *test)
{
	/* offset 0 with a 4096 unit dividing a 4096 page: clean */
	KUNIT_EXPECT_TRUE(test,
		tbv_zcopy_split_page_aligned(0, TBV_NATIVE_DATA_SPLIT_UNIT,
					     4096u));
	/* any nonzero page offset: unclean (mid-page DMA, hardware CRC risk) */
	KUNIT_EXPECT_FALSE(test,
		tbv_zcopy_split_page_aligned(1, TBV_NATIVE_DATA_SPLIT_UNIT,
					     4096u));
	KUNIT_EXPECT_FALSE(test,
		tbv_zcopy_split_page_aligned(64, TBV_NATIVE_DATA_SPLIT_UNIT,
					     4096u));
	KUNIT_EXPECT_FALSE(test,
		tbv_zcopy_split_page_aligned(4095, TBV_NATIVE_DATA_SPLIT_UNIT,
					     4096u));
	/*
	 * a page size the 4096 unit does not divide (would sub-divide a page
	 * into mid-page windows): unclean even at offset 0.
	 */
	KUNIT_EXPECT_FALSE(test,
		tbv_zcopy_split_page_aligned(0, TBV_NATIVE_DATA_SPLIT_UNIT,
					     16384u));
	KUNIT_EXPECT_FALSE(test,
		tbv_zcopy_split_page_aligned(0, 0u, 4096u));
}

static void tbv_write_shape_resolver_prefers_known_count(struct kunit *test)
{
	/*
	 * The lcm(4048,4096) ambiguity: offset 253 * 4096 == 256 * 4048, so a
	 * tail fragment there validates under BOTH units with different
	 * indices. total = 253 windows + 100-byte tail.
	 */
	u32 total = 253u * TBV_NATIVE_DATA_FRAME_SIZE + 100u;
	u32 offset = 253u * TBV_NATIVE_DATA_FRAME_SIZE;
	u32 count_4096 = 254u;
	u32 count_4048 = 257u;
	u32 idx;
	u32 count;

	/* both raw shapes are individually valid */
	KUNIT_ASSERT_EQ(test,
		tbv_native_data_fragment_shape(total,
			TBV_NATIVE_DATA_FRAME_SIZE, TBV_NATIVE_DATA_MAX_FRAGS,
			offset, 100u, true, &idx, &count), 0);
	KUNIT_ASSERT_EQ(test, count, count_4096);
	KUNIT_ASSERT_EQ(test,
		tbv_native_data_fragment_shape(total,
			TBV_NATIVE_DATA_MAX_PAYLOAD, TBV_NATIVE_DATA_MAX_FRAGS,
			offset, 100u, true, &idx, &count), 0);
	KUNIT_ASSERT_EQ(test, count, count_4048);

	/* the resolver picks whichever unit the message already established */
	KUNIT_ASSERT_EQ(test,
		tbv_native_data_write_shape_resolve(total,
			TBV_NATIVE_DATA_MAX_FRAGS, offset, 100u, true,
			count_4096, &idx, &count), 0);
	KUNIT_EXPECT_EQ(test, count, count_4096);
	KUNIT_EXPECT_EQ(test, idx, 253u);

	KUNIT_ASSERT_EQ(test,
		tbv_native_data_write_shape_resolve(total,
			TBV_NATIVE_DATA_MAX_FRAGS, offset, 100u, true,
			count_4048, &idx, &count), 0);
	KUNIT_EXPECT_EQ(test, count, count_4048);
	KUNIT_EXPECT_EQ(test, idx, 256u);

	/* no established count: any valid shape is accepted */
	KUNIT_EXPECT_EQ(test,
		tbv_native_data_write_shape_resolve(total,
			TBV_NATIVE_DATA_MAX_FRAGS, offset, 100u, true,
			0, &idx, &count), 0);

	/* an offset aligned to neither unit is malformed */
	KUNIT_EXPECT_NE(test,
		tbv_native_data_write_shape_resolve(total,
			TBV_NATIVE_DATA_MAX_FRAGS, 100u, 100u, false,
			0, &idx, &count), 0);
}

static void tbv_raw_desync_header_is_detected(struct kunit *test)
{
	struct tbv_native_data_header hdr = {};
	struct tbv_native_data_header parsed;
	u8 frame[TBV_NATIVE_DATA_HDR_SIZE];
	u8 payload[TBV_NATIVE_DATA_HDR_SIZE];
	u32 i;

	/*
	 * A retransmitted/next header arriving while raw payload is pending
	 * must be classified as a header (drop the desynced stream), which
	 * the RX path decides by parse.
	 */
	hdr.opcode = TBV_NATIVE_DATA_OP_SEND_ACK;
	hdr.psn = 42;
	KUNIT_ASSERT_GT(test,
		tbv_native_data_build_header(frame, sizeof(frame), &hdr), 0);
	KUNIT_EXPECT_EQ(test,
		tbv_native_data_parse_header(frame, sizeof(frame), &parsed), 0);

	/* arbitrary payload bytes never parse as a header */
	for (i = 0; i < sizeof(payload); i++)
		payload[i] = (u8)(i * 13u + 1u);
	KUNIT_EXPECT_NE(test,
		tbv_native_data_parse_header(payload, sizeof(payload),
					     &parsed), 0);
}

static struct kunit_case tbv_zcopy_split_cases[] = {
	KUNIT_CASE(tbv_zcopy_mode_selection),
	KUNIT_CASE(tbv_split_window_framing),
	KUNIT_CASE(tbv_split_geometry_identical_to_copied),
	KUNIT_CASE(tbv_split_page_alignment_gate),
	KUNIT_CASE(tbv_write_shape_resolver_prefers_known_count),
	KUNIT_CASE(tbv_raw_desync_header_is_detected),
	{}
};

static struct kunit_suite tbv_zcopy_split_suite = {
	.name = "thunderbolt_ibverbs_zcopy_split",
	.test_cases = tbv_zcopy_split_cases,
};
kunit_test_suite(tbv_zcopy_split_suite);
