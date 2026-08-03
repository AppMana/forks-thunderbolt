// SPDX-License-Identifier: GPL-2.0
/*
 * tbrxe_loopback_test.c - KUnit smoke test: one RC SEND through
 * requester -> loopback -> responder -> completer with a mock/null tbframe.
 *
 * The transport ops indirection (tbrxe_set_transport_ops) stands in for the
 * tbframe module, which does not exist in the KUnit kernel: a mock counts
 * every wire-path downcall and fails them, proving the whole exchange rode
 * the loopback self-path (dgid == self GID) end to end. Completions are
 * asserted on BOTH sides: the sender's CQ (requester + completer) and the
 * receiver's CQ (responder), plus payload integrity.
 *
 * Uses the module-lifetime device (tbrxe_get_dev()); module init has
 * already run when the KUnit executor starts (late_initcall vs
 * kernel_init's kunit_run_all_tests). The ib_device publishes at the FIRST
 * link_up (self GID = the identity tbframe advertised in our HELLO), so the
 * test drives a fake link_up before using the device - which also
 * re-verifies the registration-fills-GID-cache ordering (sgid_index 0 must
 * not read back -ENODATA at RTR) on the deferred-registration path.
 */

#include <kunit/test.h>
#include <linux/delay.h>
#include <rdma/ib_verbs.h>

#include "rxe.h"
#include "tbrxe_test_link.h"

#define TBRXE_TEST_MSG_LEN	256
#define TBRXE_TEST_POLL_MS	10
#define TBRXE_TEST_POLL_TRIES	500

static atomic_t mock_wire_calls;

static int mock_register_client(const struct tbframe_client_ops *ops,
				void *ctx)
{
	return 0;
}

static void mock_unregister_client(void)
{
}

static int mock_alloc_frame(struct tbframe_link *link, u16 len, bool is_ctrl,
			    struct tbframe_frame **frame)
{
	atomic_inc(&mock_wire_calls);
	return -ENETDOWN;
}

static int mock_xmit(struct tbframe_link *link, struct tbframe_frame *frame)
{
	atomic_inc(&mock_wire_calls);
	return -ENETDOWN;
}

static void mock_frame_free(struct tbframe_link *link,
			    struct tbframe_frame *frame)
{
	atomic_inc(&mock_wire_calls);
}

static const char *mock_link_name(const struct tbframe_link *link)
{
	return "mock";
}

static void mock_link_info(const struct tbframe_link *link,
			   struct tbframe_link_info *info)
{
	memset(info, 0, sizeof(*info));
}

static const struct tbrxe_transport_ops tbrxe_mock_transport = {
	.register_client	= mock_register_client,
	.unregister_client	= mock_unregister_client,
	.alloc_frame		= mock_alloc_frame,
	.xmit			= mock_xmit,
	.frame_free		= mock_frame_free,
	.link_name		= mock_link_name,
	.link_info		= mock_link_info,
};

struct tbrxe_test_side {
	struct ib_cq	*scq;
	struct ib_cq	*rcq;
	struct ib_qp	*qp;
};

static int tbrxe_test_create_side(struct kunit *test, struct ib_device *dev,
				  struct ib_pd *pd,
				  struct tbrxe_test_side *side)
{
	struct ib_cq_init_attr cq_attr = { .cqe = 16 };
	struct ib_qp_init_attr qp_attr = {
		.cap = {
			.max_send_wr	= 8,
			.max_recv_wr	= 8,
			.max_send_sge	= 2,
			.max_recv_sge	= 2,
		},
		.sq_sig_type	= IB_SIGNAL_ALL_WR,
		.qp_type	= IB_QPT_RC,
	};

	side->scq = ib_create_cq(dev, NULL, NULL, NULL, &cq_attr);
	if (IS_ERR(side->scq))
		return PTR_ERR(side->scq);

	side->rcq = ib_create_cq(dev, NULL, NULL, NULL, &cq_attr);
	if (IS_ERR(side->rcq))
		return PTR_ERR(side->rcq);

	qp_attr.send_cq = side->scq;
	qp_attr.recv_cq = side->rcq;

	side->qp = ib_create_qp(pd, &qp_attr);
	if (IS_ERR(side->qp))
		return PTR_ERR(side->qp);

	return 0;
}

static int tbrxe_test_connect(struct ib_qp *qp, u32 dest_qpn, u32 psn_local,
			      u32 psn_remote, const union ib_gid *dgid)
{
	struct ib_qp_attr attr;
	int err;

	memset(&attr, 0, sizeof(attr));
	attr.qp_state = IB_QPS_INIT;
	attr.pkey_index = 0;
	attr.port_num = 1;
	attr.qp_access_flags = IB_ACCESS_LOCAL_WRITE |
			       IB_ACCESS_REMOTE_WRITE | IB_ACCESS_REMOTE_READ;
	err = ib_modify_qp(qp, &attr, IB_QP_STATE | IB_QP_PKEY_INDEX |
			   IB_QP_PORT | IB_QP_ACCESS_FLAGS);
	if (err)
		return err;

	memset(&attr, 0, sizeof(attr));
	attr.qp_state = IB_QPS_RTR;
	attr.path_mtu = IB_MTU_2048;
	attr.dest_qp_num = dest_qpn;
	attr.rq_psn = psn_remote;
	attr.max_dest_rd_atomic = 1;
	attr.min_rnr_timer = 12;
	attr.ah_attr.type = rdma_ah_find_type(qp->device, 1);
	rdma_ah_set_port_num(&attr.ah_attr, 1);
	rdma_ah_set_grh(&attr.ah_attr, (union ib_gid *)dgid, 0, 0, 64, 0);
	err = ib_modify_qp(qp, &attr, IB_QP_STATE | IB_QP_AV |
			   IB_QP_PATH_MTU | IB_QP_DEST_QPN | IB_QP_RQ_PSN |
			   IB_QP_MAX_DEST_RD_ATOMIC | IB_QP_MIN_RNR_TIMER);
	if (err)
		return err;

	memset(&attr, 0, sizeof(attr));
	attr.qp_state = IB_QPS_RTS;
	attr.sq_psn = psn_local;
	attr.timeout = 14;
	attr.retry_cnt = 7;
	attr.rnr_retry = 7;
	attr.max_rd_atomic = 1;
	return ib_modify_qp(qp, &attr, IB_QP_STATE | IB_QP_SQ_PSN |
			    IB_QP_TIMEOUT | IB_QP_RETRY_CNT |
			    IB_QP_RNR_RETRY | IB_QP_MAX_QP_RD_ATOMIC);
}

/* Poll one completion off a CQ, sleeping between tries. */
static int tbrxe_test_poll(struct ib_cq *cq, struct ib_wc *wc)
{
	int i, n;

	for (i = 0; i < TBRXE_TEST_POLL_TRIES; i++) {
		n = ib_poll_cq(cq, 1, wc);
		if (n < 0)
			return n;
		if (n == 1)
			return 0;
		msleep(TBRXE_TEST_POLL_MS);
	}

	return -ETIMEDOUT;
}

static void tbrxe_rc_send_loopback_smoke(struct kunit *test)
{
	static int fake_link;
	struct tbrxe_test_side a = {}, b = {};
	const struct ib_recv_wr *bad_rwr;
	const struct ib_send_wr *bad_swr;
	struct ib_recv_wr rwr = {};
	struct ib_send_wr swr = {};
	struct ib_sge rsge, ssge;
	struct rxe_dev *rxe;
	struct ib_device *dev;
	struct ib_pd *pd;
	union ib_gid dgid;
	struct ib_wc wc;
	u8 *src, *dst;
	int err, i;

	rxe = tbrxe_get_dev();
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, rxe);
	dev = &rxe->ib_dev;

	atomic_set(&mock_wire_calls, 0);
	tbrxe_set_transport_ops(&tbrxe_mock_transport);

	/* First link_up publishes the ib_device and pins the self GID from
	 * the advertised local identity.
	 */
	tbrxe_test_link_up(&fake_link);

	/* The loopback target: our own self GID (driver GID table index 0,
	 * the ULA a peer would derive from our HELLO).
	 */
	err = tbrxe_query_gid(rxe, 0, &dgid);
	KUNIT_ASSERT_EQ(test, err, 0);
	KUNIT_ASSERT_EQ(test, dgid.raw[0], 0xfd);

	src = kunit_kzalloc(test, TBRXE_TEST_MSG_LEN, GFP_KERNEL);
	dst = kunit_kzalloc(test, TBRXE_TEST_MSG_LEN, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, src);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dst);
	for (i = 0; i < TBRXE_TEST_MSG_LEN; i++)
		src[i] = (u8)(i * 7 + 3);

	pd = ib_alloc_pd(dev, 0);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, pd);

	KUNIT_ASSERT_EQ(test, tbrxe_test_create_side(test, dev, pd, &a), 0);
	KUNIT_ASSERT_EQ(test, tbrxe_test_create_side(test, dev, pd, &b), 0);

	KUNIT_ASSERT_EQ(test, tbrxe_test_connect(a.qp, b.qp->qp_num,
						 0x100, 0x200, &dgid), 0);
	KUNIT_ASSERT_EQ(test, tbrxe_test_connect(b.qp, a.qp->qp_num,
						 0x200, 0x100, &dgid), 0);

	/* Receiver side. */
	rsge.addr = (uintptr_t)dst;
	rsge.length = TBRXE_TEST_MSG_LEN;
	rsge.lkey = pd->local_dma_lkey;
	rwr.wr_id = 0xb;
	rwr.sg_list = &rsge;
	rwr.num_sge = 1;
	KUNIT_ASSERT_EQ(test, ib_post_recv(b.qp, &rwr, &bad_rwr), 0);

	/* Sender side: one RC SEND. */
	ssge.addr = (uintptr_t)src;
	ssge.length = TBRXE_TEST_MSG_LEN;
	ssge.lkey = pd->local_dma_lkey;
	swr.wr_id = 0xa;
	swr.opcode = IB_WR_SEND;
	swr.send_flags = IB_SEND_SIGNALED;
	swr.sg_list = &ssge;
	swr.num_sge = 1;
	KUNIT_ASSERT_EQ(test, ib_post_send(a.qp, &swr, &bad_swr), 0);

	/* Completion on the receiver (responder ran). */
	err = tbrxe_test_poll(b.rcq, &wc);
	KUNIT_ASSERT_EQ(test, err, 0);
	KUNIT_EXPECT_EQ(test, wc.status, IB_WC_SUCCESS);
	KUNIT_EXPECT_EQ(test, wc.opcode, IB_WC_RECV);
	KUNIT_EXPECT_EQ(test, wc.byte_len, TBRXE_TEST_MSG_LEN);
	KUNIT_EXPECT_EQ(test, wc.wr_id, 0xb);

	/* Completion on the sender (ack came back, completer retired the
	 * wqe).
	 */
	err = tbrxe_test_poll(a.scq, &wc);
	KUNIT_ASSERT_EQ(test, err, 0);
	KUNIT_EXPECT_EQ(test, wc.status, IB_WC_SUCCESS);
	KUNIT_EXPECT_EQ(test, wc.opcode, IB_WC_SEND);
	KUNIT_EXPECT_EQ(test, wc.wr_id, 0xa);

	/* Payload made it through requester -> responder -> memory. */
	KUNIT_EXPECT_EQ(test, memcmp(src, dst, TBRXE_TEST_MSG_LEN), 0);

	/* The whole exchange was loopback: the mock tbframe wire path was
	 * never touched.
	 */
	KUNIT_EXPECT_EQ(test, atomic_read(&mock_wire_calls), 0);

	ib_destroy_qp(a.qp);
	ib_destroy_qp(b.qp);
	ib_destroy_cq(a.scq);
	ib_destroy_cq(a.rcq);
	ib_destroy_cq(b.scq);
	ib_destroy_cq(b.rcq);
	ib_dealloc_pd(pd);

	tbrxe_test_link_down(&fake_link);
	tbrxe_set_transport_ops(NULL);
}

static struct kunit_case tbrxe_loopback_cases[] = {
	KUNIT_CASE(tbrxe_rc_send_loopback_smoke),
	{}
};

static struct kunit_suite tbrxe_loopback_suite = {
	.name = "tbrxe_loopback",
	.test_cases = tbrxe_loopback_cases,
};

kunit_test_suites(&tbrxe_loopback_suite);
