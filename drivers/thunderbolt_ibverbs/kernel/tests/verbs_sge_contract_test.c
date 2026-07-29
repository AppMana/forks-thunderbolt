// SPDX-License-Identifier: GPL-2.0
/*
 * Pin the query_device/post_recv SGE capability contract.  A verbs provider
 * must not accept a QP configured from its advertised receive-SGE maximum and
 * then reject that same WR shape in post_recv().
 */
#include <kunit/test.h>
#include "../tbv.h"

static void tbv_test_advertised_recv_sge_is_usable(struct kunit *test)
{
	u32 send_advertised = 0;
	u32 recv_advertised = 0;
	u32 recv_accepted = 0;
	int ret;

	ret = tbv_test_verbs_sge_contract(&send_advertised,
					  &recv_advertised,
					  &recv_accepted);
	KUNIT_ASSERT_EQ(test, 0, ret);
	KUNIT_EXPECT_EQ(test, 4u, send_advertised);
	KUNIT_EXPECT_EQ(test, recv_accepted, recv_advertised);
}

static struct kunit_case tbv_verbs_sge_contract_cases[] = {
	KUNIT_CASE(tbv_test_advertised_recv_sge_is_usable),
	{}
};

static struct kunit_suite tbv_verbs_sge_contract_suite = {
	.name = "tbv_verbs_sge_contract",
	.test_cases = tbv_verbs_sge_contract_cases,
};
kunit_test_suite(tbv_verbs_sge_contract_suite);

MODULE_LICENSE("GPL");
