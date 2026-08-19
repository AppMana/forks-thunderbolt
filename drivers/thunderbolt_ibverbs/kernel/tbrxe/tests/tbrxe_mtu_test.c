// SPDX-License-Identifier: GPL-2.0
/*
 * tbrxe_mtu_test.c - the wire-spec section 5 MTU deviation.
 *
 * The fork reports IB_MTU_4096 in verbs while the engine fragments at the
 * largest 4-aligned payload whose worst-case transport unit still fits one
 * 4096-byte tbframe frame:
 *
 *	floor((4096 - 80 - 4) / 4) * 4 = 4012 bytes
 *
 * (80 = RXE_MAX_HDR_LENGTH header budget, 4 = ICRC). Both endpoints derive
 * the ceiling from the spec, so the expectations below are spec literals,
 * never implementation constants. The rxe packetization grammar is
 * unchanged in shape - FIRST/MIDDLE exactly "MTU", LAST in (0, MTU],
 * ONLY <= MTU - with the effective MTU being the deviated 4012.
 *
 * Links whose tbframe max_payload cannot carry the deviated transport unit
 * (80 + 4012 + 4 = 4096 bytes) keep the strict IB_MTU_2048 ceiling.
 */

#include <kunit/test.h>
#include <linux/delay.h>
#include <rdma/ib_verbs.h>

#include "rxe.h"
#include "tbrxe_test_link.h"

/* Spec section 5 literals. */
#define TBRXE_TEST_MTU4096_PAYLOAD	4012
/* FIRST + MIDDLE + short LAST at the deviated MTU. */
#define TBRXE_TEST_BIG_MSG_LEN		(2 * TBRXE_TEST_MTU4096_PAYLOAD + 100)

#define TBRXE_TEST_POLL_MS	10
#define TBRXE_TEST_POLL_TRIES	500

struct tbrxe_mtu_test_side {
	struct ib_cq	*scq;
	struct ib_cq	*rcq;
	struct ib_qp	*qp;
};

static int tbrxe_mtu_test_create_side(struct ib_device *dev, struct ib_pd *pd,
				      struct tbrxe_mtu_test_side *side)
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

static void tbrxe_mtu_test_destroy_side(struct tbrxe_mtu_test_side *side)
{
	if (side->qp && !IS_ERR(side->qp))
		ib_destroy_qp(side->qp);
	if (side->rcq && !IS_ERR(side->rcq))
		ib_destroy_cq(side->rcq);
	if (side->scq && !IS_ERR(side->scq))
		ib_destroy_cq(side->scq);
}

static int tbrxe_mtu_test_connect(struct ib_qp *qp, u32 dest_qpn,
				  u32 psn_local, u32 psn_remote,
				  const union ib_gid *dgid, u8 sgid_index,
				  enum ib_mtu path_mtu)
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
	attr.path_mtu = path_mtu;
	attr.dest_qp_num = dest_qpn;
	attr.rq_psn = psn_remote;
	attr.max_dest_rd_atomic = 1;
	attr.min_rnr_timer = 12;
	attr.ah_attr.type = rdma_ah_find_type(qp->device, 1);
	rdma_ah_set_port_num(&attr.ah_attr, 1);
	rdma_ah_set_grh(&attr.ah_attr, (union ib_gid *)dgid, 0,
			sgid_index, 64, 0);
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

static int tbrxe_mtu_test_poll(struct ib_cq *cq, struct ib_wc *wc)
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

/*
 * A full-size (4096-byte max_payload) link reports the deviated
 * IB_MTU_4096 in the port attributes, with the engine's byte ceiling at
 * the spec's 4012.
 */
static void tbrxe_mtu4096_port_attrs(struct kunit *test)
{
	static int fake_link;
	struct rxe_dev *rxe;

	tbrxe_test_link_up(&fake_link);
	rxe = tbrxe_test_dev(&fake_link);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, rxe);

	KUNIT_EXPECT_EQ(test, rxe->port.attr.max_mtu, IB_MTU_4096);
	KUNIT_EXPECT_EQ(test, rxe->port.attr.active_mtu, IB_MTU_4096);
	KUNIT_EXPECT_EQ(test, rxe->port.mtu_cap, TBRXE_TEST_MTU4096_PAYLOAD);

	tbrxe_test_link_down(&fake_link);
}

/*
 * A link whose max_payload cannot carry 80 + 4012 + 4 bytes keeps the
 * strict IB_MTU_2048 ceiling (spec section 5's initial rule).
 */
static void tbrxe_mtu_small_link_keeps_2048(struct kunit *test)
{
	static int fake_link;
	struct tbframe_link_info info = {
		.gid_eui64	= TBRXE_TEST_PEER_EUI64,
		.local_gid_eui64 = TBRXE_TEST_LOCAL_EUI64,
		.rx_ring_entries = 1984 + TBFRAME_CTRL_RESERVE,
		.data_window	= 1984,
		.max_payload	= 4095,
		.width		= 1,
		.speed		= 20,
		.remote_name	= "kunitpeer",
	};
	struct rxe_dev *rxe;

	tbrxe_frame_client_ops()->link_up(NULL,
					  (struct tbframe_link *)&fake_link,
					  &info);
	rxe = tbrxe_test_dev(&fake_link);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, rxe);

	KUNIT_EXPECT_EQ(test, rxe->port.attr.max_mtu, IB_MTU_2048);
	KUNIT_EXPECT_EQ(test, rxe->port.attr.active_mtu, IB_MTU_2048);
	KUNIT_EXPECT_EQ(test, rxe->port.mtu_cap, 2048);

	tbrxe_test_link_down(&fake_link);
}

/*
 * The packetization grammar at the deviated MTU: an RC SEND spanning
 * FIRST/MIDDLE/LAST fragments at exactly 4012 bytes and the responder's
 * "first or middle packet not mtu" check accepts it - both sides of the
 * deviation, requester and responder, in one exchange. The effective QP
 * MTU after RTR at IB_MTU_4096 is asserted to be the spec ceiling, not
 * the IBA 4096.
 */
static void tbrxe_mtu4096_grammar_loopback(struct kunit *test)
{
	static int fake_link;
	struct tbrxe_mtu_test_side a = {}, b = {};
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
	u8 sgid_index = 0;
	u8 *src, *dst;
	int err, i;

	tbrxe_test_link_up(&fake_link);
	rxe = tbrxe_test_dev(&fake_link);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, rxe);
	dev = &rxe->ib_dev;

	err = tbrxe_test_gid(rxe, &dgid, &sgid_index);
	KUNIT_ASSERT_EQ(test, err, 0);

	src = kunit_kzalloc(test, TBRXE_TEST_BIG_MSG_LEN, GFP_KERNEL);
	dst = kunit_kzalloc(test, TBRXE_TEST_BIG_MSG_LEN, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, src);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dst);
	for (i = 0; i < TBRXE_TEST_BIG_MSG_LEN; i++)
		src[i] = (u8)(i * 13 + 5);

	pd = ib_alloc_pd(dev, 0);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, pd);

	KUNIT_ASSERT_EQ(test, tbrxe_mtu_test_create_side(dev, pd, &a), 0);
	KUNIT_ASSERT_EQ(test, tbrxe_mtu_test_create_side(dev, pd, &b), 0);

	KUNIT_ASSERT_EQ(test, tbrxe_mtu_test_connect(a.qp, b.qp->qp_num,
						     0x100, 0x200, &dgid,
						     sgid_index,
						     IB_MTU_4096), 0);
	KUNIT_ASSERT_EQ(test, tbrxe_mtu_test_connect(b.qp, a.qp->qp_num,
						     0x200, 0x100, &dgid,
						     sgid_index,
						     IB_MTU_4096), 0);

	/* The deviation itself: enum 4096, engine ceiling 4012. */
	KUNIT_EXPECT_EQ(test, to_rqp(a.qp)->mtu, TBRXE_TEST_MTU4096_PAYLOAD);
	KUNIT_EXPECT_EQ(test, to_rqp(b.qp)->mtu, TBRXE_TEST_MTU4096_PAYLOAD);

	rsge.addr = (uintptr_t)dst;
	rsge.length = TBRXE_TEST_BIG_MSG_LEN;
	rsge.lkey = pd->local_dma_lkey;
	rwr.wr_id = 0xb;
	rwr.sg_list = &rsge;
	rwr.num_sge = 1;
	KUNIT_ASSERT_EQ(test, ib_post_recv(b.qp, &rwr, &bad_rwr), 0);

	ssge.addr = (uintptr_t)src;
	ssge.length = TBRXE_TEST_BIG_MSG_LEN;
	ssge.lkey = pd->local_dma_lkey;
	swr.wr_id = 0xa;
	swr.opcode = IB_WR_SEND;
	swr.send_flags = IB_SEND_SIGNALED;
	swr.sg_list = &ssge;
	swr.num_sge = 1;
	KUNIT_ASSERT_EQ(test, ib_post_send(a.qp, &swr, &bad_swr), 0);

	err = tbrxe_mtu_test_poll(b.rcq, &wc);
	KUNIT_ASSERT_EQ(test, err, 0);
	KUNIT_EXPECT_EQ(test, wc.status, IB_WC_SUCCESS);
	KUNIT_EXPECT_EQ(test, wc.opcode, IB_WC_RECV);
	KUNIT_EXPECT_EQ(test, wc.byte_len, TBRXE_TEST_BIG_MSG_LEN);

	err = tbrxe_mtu_test_poll(a.scq, &wc);
	KUNIT_ASSERT_EQ(test, err, 0);
	KUNIT_EXPECT_EQ(test, wc.status, IB_WC_SUCCESS);
	KUNIT_EXPECT_EQ(test, wc.opcode, IB_WC_SEND);

	KUNIT_EXPECT_EQ(test, memcmp(src, dst, TBRXE_TEST_BIG_MSG_LEN), 0);

	tbrxe_mtu_test_destroy_side(&a);
	tbrxe_mtu_test_destroy_side(&b);
	ib_dealloc_pd(pd);

	tbrxe_test_link_down(&fake_link);
}

static struct kunit_case tbrxe_mtu_cases[] = {
	KUNIT_CASE(tbrxe_mtu4096_port_attrs),
	KUNIT_CASE(tbrxe_mtu_small_link_keeps_2048),
	KUNIT_CASE(tbrxe_mtu4096_grammar_loopback),
	{}
};

static struct kunit_suite tbrxe_mtu_suite = {
	.name = "tbrxe_mtu",
	.test_cases = tbrxe_mtu_cases,
};

kunit_test_suites(&tbrxe_mtu_suite);
