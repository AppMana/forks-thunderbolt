// SPDX-License-Identifier: GPL-2.0
/*
 * tbrxe_admission_test.c - Mode A engine-side admission accounting
 * (wire-spec section 6).
 *
 * The tbframe data window only bounds frames the LOCAL TX ring has not
 * completed; TX completion says the local NHI DMA-read the frame, not that
 * the peer consumed it. Under 32 QPs x 128-PSN windows that admits ~4096
 * unacked packets against a 1984-frame peer RX ring: the ring overflows,
 * frames drop, and the PSN layer turns the loss into a retransmit storm
 * (duplicate_request ~2M in the appmana-023/025 qps32 baseline).
 *
 * The engine is the layer that sees ACKs, so it owns the invariant: the
 * aggregate unacked data packets charged against one link never exceeds the
 * advertised window, and window release is correlated with actual peer
 * consumption (the ACK proves the peer's engine processed the packet, and
 * the peer reposts the RX descriptor before its responder emits the ACK).
 *
 * This suite drives a real RC QP at a fake link whose advertised
 * data_window is tiny, with a mock transport that captures every wire
 * frame. It then feeds crafted ACKs back through the client rx upcall and
 * asserts the window opens exactly as PSNs are acknowledged.
 */

#include <kunit/test.h>
#include <linux/delay.h>
#include <rdma/ib_verbs.h>

#include "rxe.h"
#include "rxe_loc.h"
#include "tbrxe_test_link.h"

#define ADM_WINDOW		4
#define ADM_POSTS		8
#define ADM_MSG_LEN		16
#define ADM_POOL		64
#define ADM_FRAME_BYTES		512
#define ADM_SQ_PSN		0x100
#define ADM_POLL_MS		10
#define ADM_POLL_TRIES		300

static u8 adm_pool_buf[ADM_POOL][ADM_FRAME_BYTES];
static struct tbframe_frame adm_pool_frame[ADM_POOL];
static unsigned long adm_pool_used;
static DEFINE_SPINLOCK(adm_pool_lock);

/* Wire capture: PSN of every DATA (non-ack-class) packet xmitted. */
static u32 adm_sent_psn[ADM_POOL];
static atomic_t adm_sent;

static int adm_register_client(const struct tbframe_client_ops *ops, void *ctx)
{
	return 0;
}

static void adm_unregister_client(void)
{
}

static int adm_alloc_frame(struct tbframe_link *link, u16 len, bool is_ctrl,
			   struct tbframe_frame **frame)
{
	unsigned long flags;
	int i;

	if (len > ADM_FRAME_BYTES)
		return -EINVAL;

	spin_lock_irqsave(&adm_pool_lock, flags);
	i = find_first_zero_bit(&adm_pool_used, ADM_POOL);
	if (i >= ADM_POOL) {
		spin_unlock_irqrestore(&adm_pool_lock, flags);
		return -ENOSPC;
	}
	__set_bit(i, &adm_pool_used);
	spin_unlock_irqrestore(&adm_pool_lock, flags);

	adm_pool_frame[i].data = adm_pool_buf[i];
	adm_pool_frame[i].len = len;
	adm_pool_frame[i].is_ctrl = is_ctrl;
	*frame = &adm_pool_frame[i];
	return 0;
}

static void adm_frame_release(struct tbframe_frame *frame)
{
	unsigned long flags;
	int i = frame - adm_pool_frame;

	if (i < 0 || i >= ADM_POOL)
		return;
	spin_lock_irqsave(&adm_pool_lock, flags);
	__clear_bit(i, &adm_pool_used);
	spin_unlock_irqrestore(&adm_pool_lock, flags);
}

static int adm_xmit(struct tbframe_link *link, struct tbframe_frame *frame)
{
	u8 *bth = frame->data;
	u32 psn = ((u32)bth[9] << 16) | ((u32)bth[10] << 8) | bth[11];
	int n;

	/* Only count requester data packets; ack-class rides the reserve. */
	if (!frame->is_ctrl) {
		n = atomic_fetch_inc(&adm_sent);
		if (n < ADM_POOL)
			adm_sent_psn[n] = psn & BTH_PSN_MASK;
	}
	adm_frame_release(frame);
	return 0;
}

static void adm_frame_free(struct tbframe_link *link,
			   struct tbframe_frame *frame)
{
	adm_frame_release(frame);
}

static const char *adm_link_name(const struct tbframe_link *link)
{
	return "admmock";
}

static void adm_link_info(const struct tbframe_link *link,
			  struct tbframe_link_info *info)
{
	memset(info, 0, sizeof(*info));
}

static const struct tbrxe_transport_ops adm_transport = {
	.register_client	= adm_register_client,
	.unregister_client	= adm_unregister_client,
	.alloc_frame		= adm_alloc_frame,
	.xmit			= adm_xmit,
	.frame_free		= adm_frame_free,
	.link_name		= adm_link_name,
	.link_info		= adm_link_info,
};


/*
 * Craft an RC ACKNOWLEDGE for @qpn covering everything up to @psn and feed
 * it through the client rx upcall as a wire frame from the fake peer.
 */
static void adm_inject_ack(void *fake_link, u32 qpn, u32 psn)
{
	static u8 buf[RXE_BTH_BYTES + RXE_AETH_BYTES + RXE_ICRC_SIZE];
	static struct tbframe_frame frame;
	struct rxe_pkt_info pkt = {};

	memset(buf, 0, sizeof(buf));
	pkt.hdr = buf;
	pkt.opcode = IB_OPCODE_RC_ACKNOWLEDGE;
	pkt.mask = rxe_opcode[pkt.opcode].mask;
	pkt.paylen = sizeof(buf);
	pkt.psn = psn & BTH_PSN_MASK;

	bth_init(&pkt, IB_OPCODE_RC_ACKNOWLEDGE, 0, 0, 0, 0xffff,
		 qpn, 0, pkt.psn);
	aeth_set_syn(&pkt, AETH_ACK_UNLIMITED);
	aeth_set_msn(&pkt, 0);
	rxe_icrc_generate(NULL, &pkt);

	frame.data = buf;
	frame.len = sizeof(buf);
	frame.pdf = TBFRAME_PDF_DATA;
	frame.is_ctrl = false;
	tbrxe_frame_client_ops()->rx(NULL, fake_link, &frame);
}

/* Wait until the data-packet xmit count stops changing, then return it. */
static int adm_settled_sent(void)
{
	int prev = -1, cur, i;

	for (i = 0; i < ADM_POLL_TRIES; i++) {
		cur = atomic_read(&adm_sent);
		if (cur == prev)
			return cur;
		prev = cur;
		msleep(ADM_POLL_MS);
	}
	return atomic_read(&adm_sent);
}

/* Wait for the xmit count to reach at least @want (bounded), return it. */
static int adm_wait_sent(int want)
{
	int i, cur;

	for (i = 0; i < ADM_POLL_TRIES; i++) {
		cur = atomic_read(&adm_sent);
		if (cur >= want)
			break;
		msleep(ADM_POLL_MS);
	}
	/* settle so overshoot is visible too */
	return adm_settled_sent();
}

static int adm_connect(struct ib_qp *qp, u32 dest_qpn,
		       const union ib_gid *dgid, u8 sgid_index)
{
	struct ib_qp_attr attr;
	int err;

	memset(&attr, 0, sizeof(attr));
	attr.qp_state = IB_QPS_INIT;
	attr.pkey_index = 0;
	attr.port_num = 1;
	attr.qp_access_flags = IB_ACCESS_LOCAL_WRITE;
	err = ib_modify_qp(qp, &attr, IB_QP_STATE | IB_QP_PKEY_INDEX |
			   IB_QP_PORT | IB_QP_ACCESS_FLAGS);
	if (err)
		return err;

	memset(&attr, 0, sizeof(attr));
	attr.qp_state = IB_QPS_RTR;
	attr.path_mtu = IB_MTU_2048;
	attr.dest_qp_num = dest_qpn;
	attr.rq_psn = 0x300;
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
	attr.sq_psn = ADM_SQ_PSN;
	/* timeout 31 = longest retransmit timer: no retries inside the test */
	attr.timeout = 31;
	attr.retry_cnt = 7;
	attr.rnr_retry = 7;
	attr.max_rd_atomic = 1;
	return ib_modify_qp(qp, &attr, IB_QP_STATE | IB_QP_SQ_PSN |
			    IB_QP_TIMEOUT | IB_QP_RETRY_CNT |
			    IB_QP_RNR_RETRY | IB_QP_MAX_QP_RD_ATOMIC);
}

/*
 * Aggregate unacked packets on a link never exceed the advertised window;
 * ACKs (peer consumption evidence) reopen it exactly.
 */
static void tbrxe_admission_window_bounds_unacked(struct kunit *test)
{
	static int fake_link;
	struct ib_cq_init_attr cq_attr = { .cqe = 32 };
	struct ib_qp_init_attr qp_init = {
		.cap = {
			.max_send_wr	= ADM_POSTS + 2,
			.max_recv_wr	= 2,
			.max_send_sge	= 1,
			.max_recv_sge	= 1,
		},
		.sq_sig_type	= IB_SIGNAL_ALL_WR,
		.qp_type	= IB_QPT_RC,
	};
	const struct ib_send_wr *bad_swr;
	struct ib_send_wr swr = {};
	struct ib_sge ssge;
	struct rxe_dev *rxe;
	struct ib_device *dev;
	union ib_gid peer, sgid;
	u8 sgid_index = 0;
	struct ib_pd *pd;
	struct ib_cq *cq;
	struct ib_qp *qp;
	u8 *src;
	int sent, i;

	adm_pool_used = 0;
	atomic_set(&adm_sent, 0);
	tbrxe_set_transport_ops(&adm_transport);
	tbrxe_test_link_up_window(&fake_link, ADM_WINDOW);
	rxe = tbrxe_test_dev(&fake_link);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, rxe);
	dev = &rxe->ib_dev;
	KUNIT_ASSERT_EQ(test, tbrxe_test_gid(rxe, &sgid, &sgid_index), 0);
	tbrxe_test_peer_gid(&peer);

	src = kunit_kzalloc(test, ADM_MSG_LEN, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, src);

	pd = ib_alloc_pd(dev, 0);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, pd);
	cq = ib_create_cq(dev, NULL, NULL, NULL, &cq_attr);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, cq);
	qp_init.send_cq = cq;
	qp_init.recv_cq = cq;
	qp = ib_create_qp(pd, &qp_init);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, qp);

	KUNIT_ASSERT_EQ(test, adm_connect(qp, 0x42, &peer, sgid_index), 0);

	ssge.addr = (uintptr_t)src;
	ssge.length = ADM_MSG_LEN;
	ssge.lkey = pd->local_dma_lkey;
	swr.opcode = IB_WR_SEND;
	swr.send_flags = IB_SEND_SIGNALED;
	swr.sg_list = &ssge;
	swr.num_sge = 1;
	for (i = 0; i < ADM_POSTS; i++) {
		swr.wr_id = i;
		KUNIT_ASSERT_EQ(test, ib_post_send(qp, &swr, &bad_swr), 0);
	}

	/*
	 * 8 one-packet WRs against a window of 4 with no ACKs: exactly 4
	 * packets may reach the wire.
	 */
	sent = adm_settled_sent();
	KUNIT_EXPECT_EQ(test, sent, ADM_WINDOW);
	for (i = 0; i < min(sent, ADM_WINDOW); i++)
		KUNIT_EXPECT_EQ(test, adm_sent_psn[i],
				(u32)(ADM_SQ_PSN + i) & BTH_PSN_MASK);

	/* ACK the first two PSNs: exactly two more slots open. */
	adm_inject_ack(&fake_link, qp->qp_num, ADM_SQ_PSN + 1);
	sent = adm_wait_sent(ADM_WINDOW + 2);
	KUNIT_EXPECT_EQ(test, sent, ADM_WINDOW + 2);

	/* ACK everything: the remaining packets drain. */
	adm_inject_ack(&fake_link, qp->qp_num, ADM_SQ_PSN + ADM_POSTS - 1);
	sent = adm_wait_sent(ADM_POSTS);
	KUNIT_EXPECT_EQ(test, sent, ADM_POSTS);

	ib_destroy_qp(qp);
	ib_destroy_cq(cq);
	ib_dealloc_pd(pd);
	tbrxe_test_link_down(&fake_link);
	tbrxe_set_transport_ops(NULL);
}

/*
 * A QP torn down with the window fully charged must return its charge:
 * a second QP on the same link starts with the full window available.
 */
static void tbrxe_admission_charge_released_on_destroy(struct kunit *test)
{
	static int fake_link;
	struct ib_cq_init_attr cq_attr = { .cqe = 32 };
	struct ib_qp_init_attr qp_init = {
		.cap = {
			.max_send_wr	= ADM_POSTS + 2,
			.max_recv_wr	= 2,
			.max_send_sge	= 1,
			.max_recv_sge	= 1,
		},
		.sq_sig_type	= IB_SIGNAL_ALL_WR,
		.qp_type	= IB_QPT_RC,
	};
	const struct ib_send_wr *bad_swr;
	struct ib_send_wr swr = {};
	struct ib_sge ssge;
	struct rxe_dev *rxe;
	struct ib_device *dev;
	union ib_gid peer, sgid;
	u8 sgid_index = 0;
	struct ib_pd *pd;
	struct ib_cq *cq;
	struct ib_qp *qp;
	u8 *src;
	int sent, i;

	adm_pool_used = 0;
	atomic_set(&adm_sent, 0);
	tbrxe_set_transport_ops(&adm_transport);
	tbrxe_test_link_up_window(&fake_link, ADM_WINDOW);
	rxe = tbrxe_test_dev(&fake_link);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, rxe);
	dev = &rxe->ib_dev;
	KUNIT_ASSERT_EQ(test, tbrxe_test_gid(rxe, &sgid, &sgid_index), 0);
	tbrxe_test_peer_gid(&peer);

	src = kunit_kzalloc(test, ADM_MSG_LEN, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, src);
	pd = ib_alloc_pd(dev, 0);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, pd);
	cq = ib_create_cq(dev, NULL, NULL, NULL, &cq_attr);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, cq);
	qp_init.send_cq = cq;
	qp_init.recv_cq = cq;

	/* First QP: fill the window, never ack, destroy. */
	qp = ib_create_qp(pd, &qp_init);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, qp);
	KUNIT_ASSERT_EQ(test, adm_connect(qp, 0x42, &peer, sgid_index), 0);

	ssge.addr = (uintptr_t)src;
	ssge.length = ADM_MSG_LEN;
	ssge.lkey = pd->local_dma_lkey;
	swr.opcode = IB_WR_SEND;
	swr.send_flags = IB_SEND_SIGNALED;
	swr.sg_list = &ssge;
	swr.num_sge = 1;
	for (i = 0; i < ADM_POSTS; i++) {
		swr.wr_id = i;
		KUNIT_ASSERT_EQ(test, ib_post_send(qp, &swr, &bad_swr), 0);
	}
	sent = adm_settled_sent();
	KUNIT_EXPECT_EQ(test, sent, ADM_WINDOW);
	ib_destroy_qp(qp);

	/* Second QP: the full window must be available again. */
	atomic_set(&adm_sent, 0);
	qp = ib_create_qp(pd, &qp_init);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, qp);
	KUNIT_ASSERT_EQ(test, adm_connect(qp, 0x43, &peer, sgid_index), 0);
	for (i = 0; i < ADM_WINDOW; i++) {
		swr.wr_id = i;
		KUNIT_ASSERT_EQ(test, ib_post_send(qp, &swr, &bad_swr), 0);
	}
	sent = adm_settled_sent();
	KUNIT_EXPECT_EQ(test, sent, ADM_WINDOW);

	ib_destroy_qp(qp);
	ib_destroy_cq(cq);
	ib_dealloc_pd(pd);
	tbrxe_test_link_down(&fake_link);
	tbrxe_set_transport_ops(NULL);
}

static struct kunit_case tbrxe_admission_cases[] = {
	KUNIT_CASE(tbrxe_admission_window_bounds_unacked),
	KUNIT_CASE(tbrxe_admission_charge_released_on_destroy),
	{}
};

static struct kunit_suite tbrxe_admission_suite = {
	.name = "tbrxe_admission",
	.test_cases = tbrxe_admission_cases,
};

kunit_test_suites(&tbrxe_admission_suite);
