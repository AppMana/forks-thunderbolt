// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit: the usb4_rdma write()-ABI data-path dispatch invariant.
 *
 * The userspace provider posts via ibv_cmd_post_{send,recv} (the write() uverbs
 * ABI). The kernel routes those write() commands to the driver's
 * .post_{send,recv}/.poll_cq/.req_notify_cq ops ONLY when BOTH hold:
 *   (a) the op is registered in tbv_ibdev_ops, and
 *   (b) the matching uverbs_cmd_mask bit is set (tbv_ibdev_uverbs_cmd_mask()).
 * Dropping either half reopens the historical ENOSYS-on-post_recv hole: the
 * provider's ibv_cmd_post_recv -> write() returns ENOSYS, and NCCL retry-spins
 * for ~600s. The 6.17 kernel supplies the write() handlers + dispatch entries
 * (uverbs_cmd.c POST_RECV/POST_SEND/POLL_CQ/REQ_NOTIFY_CQ); this test pins the
 * DRIVER's two halves so the path can't silently break.
 *
 * Built into the module on CONFIG_KUNIT; run via tools/run-kunit.sh.
 */
#include <kunit/test.h>
#include <linux/bitops.h>
#include <rdma/ib_verbs.h>
#include <uapi/rdma/ib_user_verbs.h>
#include "../tbv.h"

/* (a) the data-path ops the kernel write() dispatch calls into are registered */
static void tbv_test_datapath_ops_registered(struct kunit *test)
{
	const struct ib_device_ops *ops = tbv_ibdev_ops_ref();

	KUNIT_ASSERT_TRUE(test, ops != NULL);
	KUNIT_EXPECT_TRUE(test, ops->post_send != NULL);
	KUNIT_EXPECT_TRUE(test, ops->post_recv != NULL);
	KUNIT_EXPECT_TRUE(test, ops->poll_cq != NULL);
	KUNIT_EXPECT_TRUE(test, ops->req_notify_cq != NULL);
}

/* (b) the matching write()-command mask bits are enabled */
static void tbv_test_datapath_cmd_mask_bits(struct kunit *test)
{
	u64 m = tbv_ibdev_uverbs_cmd_mask();

	KUNIT_EXPECT_TRUE(test, (m & BIT_ULL(IB_USER_VERBS_CMD_POST_SEND)) != 0);
	KUNIT_EXPECT_TRUE(test, (m & BIT_ULL(IB_USER_VERBS_CMD_POST_RECV)) != 0);
	KUNIT_EXPECT_TRUE(test, (m & BIT_ULL(IB_USER_VERBS_CMD_POLL_CQ)) != 0);
	KUNIT_EXPECT_TRUE(test, (m & BIT_ULL(IB_USER_VERBS_CMD_REQ_NOTIFY_CQ)) != 0);
}

static struct kunit_case tbv_post_recv_dispatch_cases[] = {
	KUNIT_CASE(tbv_test_datapath_ops_registered),
	KUNIT_CASE(tbv_test_datapath_cmd_mask_bits),
	{}
};

static struct kunit_suite tbv_post_recv_dispatch_suite = {
	.name = "thunderbolt_ibverbs_post_recv_dispatch",
	.test_cases = tbv_post_recv_dispatch_cases,
};
kunit_test_suite(tbv_post_recv_dispatch_suite);
