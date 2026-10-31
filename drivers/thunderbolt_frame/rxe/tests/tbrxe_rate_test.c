// SPDX-License-Identifier: GPL-2.0
/*
 * tbrxe_rate_test.c - the IB port rate a Thunderbolt rail advertises.
 *
 * Consumers size their cost model from active_speed x active_width. NCCL
 * computes exactly ncclIbSpeed(active_speed) * ncclIbWidth(active_width),
 * with the tables (src/transport/net_ib/init.cc):
 *
 *	ibvWidths[] = {1, 4, 8, 12, 2}   indexed by firstBitSet(width)
 *	ibvSpeeds[] = {2500, 5000, 10000, 10000, ...}
 *
 * so IB_WIDTH_1X->1, 4X->4, 8X->8, 12X->12, 2X->2, and SDR->2500,
 * DDR->5000, QDR->10000 Mb/s.
 *
 * Leaving the scaffold's SDR/1X in place advertised 2500 Mb/s for a 20 Gb/s
 * rail while stock rxe_lan reports a nominal 10 Gb/s over a 2.5GbE NIC, so
 * every rail ranked below the LAN. Measured on the live chain 2026-08-25:
 * NCCL logged "Made virtual device [0] name=rxe_lan speed=10000" against
 * "[1] name=usb4_rdma0 speed=2500", and placed both of appmana-021's PP
 * hops on rxe_lan even though its rails measured 1472 and 1634 MB/s.
 *
 * The expectations below are the wire truth (aggregate Gb/s of the link),
 * expressed through whatever encoding reproduces it, never implementation
 * constants.
 */

#include <kunit/test.h>
#include <rdma/ib_verbs.h>

#include "rxe.h"

/* Mirror of NCCL's tables, so the assertions are about the number a
 * consumer actually derives rather than about our chosen encoding.
 */
static int nccl_width(u8 active_width)
{
	switch (active_width) {
	case IB_WIDTH_1X:	return 1;
	case IB_WIDTH_4X:	return 4;
	case IB_WIDTH_8X:	return 8;
	case IB_WIDTH_12X:	return 12;
	case IB_WIDTH_2X:	return 2;
	default:		return -1;
	}
}

static int nccl_speed(u8 active_speed)
{
	switch (active_speed) {
	case IB_SPEED_SDR:	return 2500;
	case IB_SPEED_DDR:	return 5000;
	case IB_SPEED_QDR:	return 10000;
	case IB_SPEED_FDR10:	return 10000;
	default:		return -1;
	}
}

static int derived_mbps(u8 tb_speed_gbps, u8 tb_lanes)
{
	u8 speed, width;

	tbrxe_link_ib_rate(tb_speed_gbps, tb_lanes, &speed, &width);
	return nccl_speed(speed) * nccl_width(width);
}

/*
 * The case that matters on this fleet: Thunderbolt 4, one lane, 20 Gb/s.
 * Before the fix this derived 2500 -- an eighth of the truth, and a quarter
 * of what rxe_lan claims, which is why the rails went unused.
 */
static void tbrxe_rate_tb4_single_lane_is_20g(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, 20000, derived_mbps(20, 1));
}

static void tbrxe_rate_bonded_pair_is_40g(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, 40000, derived_mbps(20, 2));
}

static void tbrxe_rate_tb3_single_lane_is_10g(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, 10000, derived_mbps(10, 1));
}

/*
 * A rail must never rank below stock rxe_lan, which reports a nominal
 * 10 Gb/s (FDR10 x 1X) regardless of the NIC underneath it. This is the
 * property the whole fix exists to establish; assert it directly rather
 * than only through the individual encodings above.
 */
static void tbrxe_rate_outranks_rxe_lan(struct kunit *test)
{
	int rxe_lan_mbps = nccl_speed(IB_SPEED_FDR10) * nccl_width(IB_WIDTH_1X);

	KUNIT_EXPECT_EQ(test, 10000, rxe_lan_mbps);
	KUNIT_EXPECT_GT(test, derived_mbps(20, 1), rxe_lan_mbps);
	KUNIT_EXPECT_GT(test, derived_mbps(20, 2), rxe_lan_mbps);
}

/*
 * Width 0 means "not reported" rather than "no lanes"; treat it as one
 * lane instead of collapsing the rate to zero.
 */
static void tbrxe_rate_unreported_width_is_one_lane(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, derived_mbps(20, 0), derived_mbps(20, 1));
}

/*
 * Below one QDR lane there is no encoding that tells the truth, so keep the
 * conservative scaffold floor rather than inventing bandwidth.
 */
static void tbrxe_rate_sub_qdr_keeps_the_floor(struct kunit *test)
{
	u8 speed, width;

	tbrxe_link_ib_rate(0, 0, &speed, &width);
	KUNIT_EXPECT_EQ(test, (int)RXE_PORT_ACTIVE_SPEED, (int)speed);
	KUNIT_EXPECT_EQ(test, (int)RXE_PORT_ACTIVE_WIDTH, (int)width);
}

static struct kunit_case tbrxe_rate_cases[] = {
	KUNIT_CASE(tbrxe_rate_tb4_single_lane_is_20g),
	KUNIT_CASE(tbrxe_rate_bonded_pair_is_40g),
	KUNIT_CASE(tbrxe_rate_tb3_single_lane_is_10g),
	KUNIT_CASE(tbrxe_rate_outranks_rxe_lan),
	KUNIT_CASE(tbrxe_rate_unreported_width_is_one_lane),
	KUNIT_CASE(tbrxe_rate_sub_qdr_keeps_the_floor),
	{}
};

static struct kunit_suite tbrxe_rate_suite = {
	.name = "tbrxe_rate",
	.test_cases = tbrxe_rate_cases,
};
kunit_test_suites(&tbrxe_rate_suite);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("KUnit tests for the tbrxe IB port rate derivation");
