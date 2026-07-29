// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit: a QP that dies must tell the consumer.
 *
 * A verbs consumer watching the async fd (NCCL's ncclIbAsyncThread) learns
 * that a QP failed only from an asynchronous event. The driver failed QPs
 * silently, so a collective saw nothing until every WR on the QP had timed out
 * on its own -- and a QP whose WRs are stalled rather than completing produces
 * no completions to time out against either.
 *
 * These run the real transition (tbv_qp_mark_error_event) and the real CQ push
 * with handlers installed where ib_uverbs installs them. See the hook in
 * ibdev.c for the provenance of the QP and CQ state it builds.
 *
 * Built into the module on CONFIG_KUNIT; run via tools/run-kunit.sh.
 */
#include <kunit/test.h>
#include <rdma/ib_verbs.h>
#include "../tbv.h"

static void tbv_async_qp_fatal_is_reported(struct kunit *test)
{
	u32 qp_events = 0;
	u32 cq_events = 0;
	int qp_event = -1;
	int cq_event = -1;

	KUNIT_ASSERT_EQ(test,
			tbv_test_qp_async_events(false, false, &qp_events,
						 &qp_event, &cq_events,
						 &cq_event),
			0);

	KUNIT_EXPECT_EQ_MSG(test, qp_events, 1U,
			    "a failed QP must raise exactly one async event");
	KUNIT_EXPECT_EQ(test, qp_event, (int)IB_EVENT_QP_FATAL);
}

/* A requester-side failure names itself so the consumer can tell them apart. */
static void tbv_async_qp_req_err_is_reported(struct kunit *test)
{
	u32 qp_events = 0;
	u32 cq_events = 0;
	int qp_event = -1;
	int cq_event = -1;

	KUNIT_ASSERT_EQ(test,
			tbv_test_qp_async_events(true, false, &qp_events,
						 &qp_event, &cq_events,
						 &cq_event),
			0);

	KUNIT_EXPECT_EQ(test, qp_events, 1U);
	KUNIT_EXPECT_EQ(test, qp_event, (int)IB_EVENT_QP_REQ_ERR);
}

/*
 * An overflowed CQ has silently dropped a completion, which no consumer can
 * detect by polling. It must raise IB_EVENT_CQ_ERR, and only on the
 * transition -- a CQ that stays overflowed must not storm the async fd.
 */
static void tbv_async_cq_overflow_is_reported(struct kunit *test)
{
	u32 qp_events = 0;
	u32 cq_events = 0;
	int qp_event = -1;
	int cq_event = -1;

	KUNIT_ASSERT_EQ(test,
			tbv_test_qp_async_events(false, true, &qp_events,
						 &qp_event, &cq_events,
						 &cq_event),
			0);

	KUNIT_EXPECT_EQ_MSG(test, cq_events, 1U,
			    "CQ overflow must be reported exactly once");
	KUNIT_EXPECT_EQ(test, cq_event, (int)IB_EVENT_CQ_ERR);
}

static struct kunit_case tbv_async_event_cases[] = {
	KUNIT_CASE(tbv_async_qp_fatal_is_reported),
	KUNIT_CASE(tbv_async_qp_req_err_is_reported),
	KUNIT_CASE(tbv_async_cq_overflow_is_reported),
	{}
};

static struct kunit_suite tbv_async_event_suite = {
	.name = "thunderbolt_ibverbs_async_event",
	.test_cases = tbv_async_event_cases,
};

kunit_test_suite(tbv_async_event_suite);
