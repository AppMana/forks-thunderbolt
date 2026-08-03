// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit: native rings use hardware E2E and software admission together.
 *
 * E2E prevents the NHI/router FIFO from overrunning the peer, but completion
 * only means the local NHI accepted a descriptor. Without the bounded software
 * PATH_CREDIT window, a deep verbs SQ can queue seconds of data behind E2E and
 * every later WR times out and retransmits before its ACK can return. Hardware
 * E2E is fabric safety; software credits are transport admission. They are not
 * substitutes for one another.
 */
#include <kunit/test.h>
#include "../../proto/native_data.h"
#include "../../proto/native_wire.h"
#include "../tbv.h"

static void tbv_native_path_defaults_to_software_credits(struct kunit *test)
{
	struct tbv_path_config cfg;

	tbv_path_default_config(TBV_BACKEND_NATIVE, &cfg);

	KUNIT_EXPECT_FALSE(test, cfg.e2e);
	KUNIT_EXPECT_TRUE(test, cfg.tx_flags & RING_FLAG_FRAME);
	KUNIT_EXPECT_TRUE(test, cfg.rx_flags & RING_FLAG_FRAME);
	KUNIT_EXPECT_FALSE(test, cfg.tx_flags & RING_FLAG_E2E);
	KUNIT_EXPECT_FALSE(test, cfg.rx_flags & RING_FLAG_E2E);
}

static void tbv_native_path_defaults_to_low_latency_pacing(struct kunit *test)
{
	/*
	 * 023<->025, 32 QPs x depth 128 x 256 KiB bidirectional: 32 NHI
	 * descriptors in flight corrupts full-duplex frames, while one completes
	 * three rounds at the same 3.6 Gb/s with no new CRC, header, retry, or
	 * timeout counters. One also removes avoidable hardware queueing latency.
	 */
	KUNIT_EXPECT_EQ(test, TBV_DATA_TX_MAX_INFLIGHT_DEFAULT, 1U);
}

static void tbv_e2e_peers_keep_software_credits(struct kunit *test)
{
	u32 credits = 0;
	u32 max = 0;
	int ret;

	ret = tbv_test_path_credit_mode(
		true, true, TBV_NATIVE_WIRE_CAP_E2E_NO_SW_CREDIT,
		&credits, &max);

	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_GT_MSG(test, max, 0u,
		"hardware E2E incorrectly disabled software path admission");
	KUNIT_EXPECT_EQ(test, credits, max);
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

static void tbv_copied_packet_owns_inline_payload(struct kunit *test)
{
	u32 fill_calls = 0;
	bool inline_buf = false;
	bool data_match = false;
	int ret;

	ret = tbv_test_path_inline_prepare(TBV_NATIVE_DATA_FRAME_SIZE,
					   &fill_calls, &inline_buf,
					   &data_match);

	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, fill_calls, 1u);
	KUNIT_EXPECT_TRUE(test, inline_buf);
	KUNIT_EXPECT_TRUE(test, data_match);
}

static struct kunit_case tbv_path_config_cases[] = {
	KUNIT_CASE(tbv_native_path_defaults_to_software_credits),
	KUNIT_CASE(tbv_native_path_defaults_to_low_latency_pacing),
	KUNIT_CASE(tbv_e2e_peers_keep_software_credits),
	KUNIT_CASE(tbv_old_peer_keeps_software_credits),
	KUNIT_CASE(tbv_non_e2e_peer_keeps_software_credits),
	KUNIT_CASE(tbv_non_e2e_local_keeps_software_credits),
	KUNIT_CASE(tbv_copied_packet_owns_inline_payload),
	{}
};

static struct kunit_suite tbv_path_config_suite = {
	.name = "thunderbolt_ibverbs_path_config",
	.test_cases = tbv_path_config_cases,
};
kunit_test_suite(tbv_path_config_suite);
