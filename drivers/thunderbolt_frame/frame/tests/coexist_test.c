// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit: tbnet + tbframe single-tunnel coexistence invariant.
 *
 * Under a resident ICM connection manager the fabric enables exactly one DMA
 * tunnel per XDomain (register-proven on the fleet: on any dual-service link
 * exactly one lane-port in_hop carries the enable bit, the first approver's;
 * the loser is a silent 0-RX zombie). True coexistence therefore multiplexes
 * both services onto the single approved tunnel, demultiplexed by the
 * frame-type (EOF) PDF nibble. That is only sound if the two services' PDF
 * namespaces are disjoint and the shared RX ring admits both. This suite pins
 * those invariants so an implementation cannot regress into misrouting or into
 * silently dropping one service's frames.
 *
 * Run via tools/run-kunit.sh 'tbframe_coexist'.
 */
#include <kunit/test.h>
#include <linux/bits.h>

#include "../tbframe_priv.h"

/* The whole point: a frame is attributable to exactly one service by its PDF. */
static void coexist_pdf_namespaces_disjoint(struct kunit *test)
{
	u16 frame_bits = TBFRAME_SOF_MASK | TBFRAME_EOF_MASK;
	u16 tbnet_bits = TBNET_SOF_MASK | TBNET_EOF_MASK;

	KUNIT_EXPECT_EQ_MSG(test, frame_bits & tbnet_bits, 0,
			    "tbframe and tbnet PDF namespaces overlap: %#x & %#x",
			    frame_bits, tbnet_bits);
}

/* The shared ring must open frames for both services (union of SOF masks). */
static void coexist_shared_sof_mask_is_union(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, TBFRAME_SHARED_SOF_MASK,
			(u16)(TBFRAME_SOF_MASK | TBNET_SOF_MASK));
	KUNIT_EXPECT_TRUE_MSG(test, TBFRAME_SHARED_SOF_MASK & TBNET_SOF_MASK,
			      "shared SOF mask drops tbnet frame-start PDF");
	KUNIT_EXPECT_TRUE_MSG(test, TBFRAME_SHARED_SOF_MASK & TBFRAME_SOF_MASK,
			      "shared SOF mask drops tbframe SOF marker");
}

/* The shared ring must close frames for both services (union of EOF masks). */
static void coexist_shared_eof_mask_is_union(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, TBFRAME_SHARED_EOF_MASK,
			(u16)(TBFRAME_EOF_MASK | TBNET_EOF_MASK));
	KUNIT_EXPECT_TRUE_MSG(test, TBFRAME_SHARED_EOF_MASK & TBNET_EOF_MASK,
			      "shared EOF mask drops tbnet frame-end PDF (0 RX for IP)");
	KUNIT_EXPECT_TRUE_MSG(test, TBFRAME_SHARED_EOF_MASK & BIT(TBFRAME_PDF_DATA),
			      "shared EOF mask drops tbframe data PDF (RDMA regressed)");
}

/* Demux: each service's frame-type PDF resolves to that service. */
static void coexist_demux_routes_each_service(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, tbframe_pdf_class(TBFRAME_PDF_DATA),
			TBFRAME_PDF_CLASS_TBFRAME);
	KUNIT_EXPECT_EQ(test, tbframe_pdf_class(TBFRAME_PDF_KEEPALIVE),
			TBFRAME_PDF_CLASS_TBFRAME);
	KUNIT_EXPECT_EQ(test, tbframe_pdf_class(TBNET_PDF_FRAME_END),
			TBFRAME_PDF_CLASS_TBNET);
}

/*
 * The SOF marker (0x6) is a frame-open sentinel, never a frame type: the NHI
 * writes the closing packet's PDF into the RX descriptor's EOF field, so a
 * frame is classified by its EOF, and the SOF value must not be mistaken for a
 * deliverable class. Unknown PDFs are dropped, not misrouted.
 */
static void coexist_demux_rejects_non_frame_types(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, tbframe_pdf_class(TBFRAME_PDF_SOF),
			TBFRAME_PDF_CLASS_NONE);
	KUNIT_EXPECT_EQ(test, tbframe_pdf_class(0),
			TBFRAME_PDF_CLASS_NONE);
	KUNIT_EXPECT_EQ(test, tbframe_pdf_class(0xf),
			TBFRAME_PDF_CLASS_NONE);
}

/* No PDF may map to two services: the demux is unambiguous across all 16. */
static void coexist_demux_unambiguous(struct kunit *test)
{
	unsigned int pdf;

	for (pdf = 0; pdf < 16; pdf++) {
		enum tbframe_pdf_class c = tbframe_pdf_class(pdf);
		bool in_frame = BIT(pdf) & TBFRAME_EOF_MASK;
		bool in_tbnet = BIT(pdf) & TBNET_EOF_MASK;

		KUNIT_EXPECT_FALSE_MSG(test, in_frame && in_tbnet,
				       "PDF %u claimed by both services", pdf);
		if (in_frame)
			KUNIT_EXPECT_EQ(test, c, TBFRAME_PDF_CLASS_TBFRAME);
		else if (in_tbnet)
			KUNIT_EXPECT_EQ(test, c, TBFRAME_PDF_CLASS_TBNET);
		else
			KUNIT_EXPECT_EQ(test, c, TBFRAME_PDF_CLASS_NONE);
	}
}

static struct kunit_case tbframe_coexist_cases[] = {
	KUNIT_CASE(coexist_pdf_namespaces_disjoint),
	KUNIT_CASE(coexist_shared_sof_mask_is_union),
	KUNIT_CASE(coexist_shared_eof_mask_is_union),
	KUNIT_CASE(coexist_demux_routes_each_service),
	KUNIT_CASE(coexist_demux_rejects_non_frame_types),
	KUNIT_CASE(coexist_demux_unambiguous),
	{}
};

static struct kunit_suite tbframe_coexist_suite = {
	.name = "tbframe_coexist",
	.test_cases = tbframe_coexist_cases,
};
kunit_test_suite(tbframe_coexist_suite);

MODULE_LICENSE("GPL");
