// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit: native rings use the NHI's hardware end-to-end credit path.
 *
 * Software PATH_CREDIT bounds the number of frames accepted by the protocol,
 * but the 8-QP hardware capture still showed the sender complete 127k data
 * descriptors while the peer consumed only 112k, with no RX overrun or CRC
 * status. E2E ring credits prevent the NHI/router FIFO from silently dropping
 * accepted descriptors when bidirectional traffic fills the receive side.
 */
#include <kunit/test.h>
#include "../tbv.h"

static void tbv_native_path_enables_hardware_e2e(struct kunit *test)
{
	struct tbv_path_config cfg;

	tbv_path_default_config(TBV_BACKEND_NATIVE, &cfg);

	KUNIT_EXPECT_TRUE(test, cfg.e2e);
	KUNIT_EXPECT_TRUE(test, cfg.tx_flags & RING_FLAG_FRAME);
	KUNIT_EXPECT_TRUE(test, cfg.rx_flags & RING_FLAG_FRAME);
	KUNIT_EXPECT_TRUE(test, cfg.tx_flags & RING_FLAG_E2E);
	KUNIT_EXPECT_TRUE(test, cfg.rx_flags & RING_FLAG_E2E);
}

static struct kunit_case tbv_path_config_cases[] = {
	KUNIT_CASE(tbv_native_path_enables_hardware_e2e),
	{}
};

static struct kunit_suite tbv_path_config_suite = {
	.name = "thunderbolt_ibverbs_path_config",
	.test_cases = tbv_path_config_cases,
};
kunit_test_suite(tbv_path_config_suite);
