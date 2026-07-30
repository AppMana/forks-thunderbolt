// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit: native rings use the NHI's hardware end-to-end credit path.
 *
 * E2E ring credits prevent the NHI/router FIFO from overrunning the peer under
 * bidirectional load. New peers use that hardware mechanism directly; the
 * software PATH_CREDIT window remains only for mixed-version/non-E2E paths.
 */
#include <kunit/test.h>
#include "../../proto/native_wire.h"
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

static void tbv_e2e_peers_bypass_software_credits(struct kunit *test)
{
	u32 credits = U32_MAX;
	u32 max = U32_MAX;
	int ret;

	ret = tbv_test_path_credit_mode(
		true, true, TBV_NATIVE_WIRE_CAP_E2E_NO_SW_CREDIT,
		&credits, &max);

	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, credits, 0u);
	KUNIT_EXPECT_EQ(test, max, 0u);
}

static void tbv_old_peer_keeps_software_credits(struct kunit *test)
{
	u32 credits = 0;
	u32 max = 0;
	int ret;

	ret = tbv_test_path_credit_mode(true, true, 0, &credits, &max);

	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_GT(test, max, 0u);
	KUNIT_EXPECT_EQ(test, credits, max);
}

static void tbv_non_e2e_peer_keeps_software_credits(struct kunit *test)
{
	u32 credits = 0;
	u32 max = 0;
	int ret;

	ret = tbv_test_path_credit_mode(
		true, false, TBV_NATIVE_WIRE_CAP_E2E_NO_SW_CREDIT,
		&credits, &max);

	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_GT(test, max, 0u);
	KUNIT_EXPECT_EQ(test, credits, max);
}

static void tbv_non_e2e_local_keeps_software_credits(struct kunit *test)
{
	u32 credits = 0;
	u32 max = 0;
	int ret;

	ret = tbv_test_path_credit_mode(
		false, true, TBV_NATIVE_WIRE_CAP_E2E_NO_SW_CREDIT,
		&credits, &max);

	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_GT(test, max, 0u);
	KUNIT_EXPECT_EQ(test, credits, max);
}

static struct kunit_case tbv_path_config_cases[] = {
	KUNIT_CASE(tbv_native_path_enables_hardware_e2e),
	KUNIT_CASE(tbv_e2e_peers_bypass_software_credits),
	KUNIT_CASE(tbv_old_peer_keeps_software_credits),
	KUNIT_CASE(tbv_non_e2e_peer_keeps_software_credits),
	KUNIT_CASE(tbv_non_e2e_local_keeps_software_credits),
	{}
};

static struct kunit_suite tbv_path_config_suite = {
	.name = "thunderbolt_ibverbs_path_config",
	.test_cases = tbv_path_config_cases,
};
kunit_test_suite(tbv_path_config_suite);
