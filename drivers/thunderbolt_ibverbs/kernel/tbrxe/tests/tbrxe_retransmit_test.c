// SPDX-License-Identifier: GPL-2.0
/*
 * tbrxe_retransmit_test.c - bounded first-retransmit delay on a sub-ms RTT
 * fabric (the legacy 0.2.17/0.2.18 retransmit-timeout mechanism, tbrxe
 * shape).
 *
 * The IB ack_timeout curve (4.096us * 2^t) is sized for WAN fabrics: NCCL
 * passes t=18 (~1.07 s), perftest t=14 (~67 ms), while the measured RTT on
 * the canary link is ~15 us. The retransmit timer's only job here is
 * recovering the ~2-4e-6/frame CRC loss the PSN layer sees, so a stock
 * timeout turns every lost frame into a 67ms-1s stall - the measured
 * "100x-slow NCCL iteration" tail. The engine therefore arms the retry
 * timer at an exponential-backoff delay: base << consecutive-timeouts,
 * capped at the verbs ack_timeout (the verbs value stays the ceiling, so a
 * consumer that asked for a long timeout never waits longer than it asked).
 * Backoff, not a flat clamp: the legacy flat 5 ms cap made total budget =
 * interval * retry_cnt, which starved real recoveries and killed QPs
 * (legacy 0.2.18 fix); doubling keeps the first retransmit fast while the
 * cumulative budget stays unbounded-ish. Any response packet from the peer
 * resets the backoff.
 *
 * The behavioral tests drive a real RC QP at a mock transport that
 * captures xmit timestamps and simply never delivers, then measure the
 * requester's actual retransmit spacing.
 */

#include <kunit/test.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <rdma/ib_verbs.h>

#include "rxe.h"
#include "rxe_loc.h"
#include "tbrxe_test_link.h"

#define RTX_MSG_LEN		16
#define RTX_POOL		64
#define RTX_FRAME_BYTES		512
#define RTX_SQ_PSN		0x100
#define RTX_POLL_MS		5
#define RTX_POLL_TRIES		600
/* Verbs ack_timeout for the tests: t=17 -> 4.096us << 17 = ~549 ms. */
#define RTX_VERBS_TIMEOUT	17
/*
 * A first retransmit is "prompt" when it lands well under the verbs curve.
 * Generous bound for qemu scheduling noise: the backoff base is 10 ms, the
 * stock behavior is ~549 ms at t=17.
 */
#define RTX_PROMPT_MS		150

static u8 rtx_pool_buf[RTX_POOL][RTX_FRAME_BYTES];
static struct tbframe_frame rtx_pool_frame[RTX_POOL];
static unsigned long rtx_pool_used;
static DEFINE_SPINLOCK(rtx_pool_lock);

/* Wire capture: jiffies timestamp of every DATA packet xmit. */
static unsigned long rtx_sent_at[RTX_POOL];
static atomic_t rtx_sent;

static int rtx_register_client(const struct tbframe_client_ops *ops,
			       void *ctx)
{
	return 0;
}

static void rtx_unregister_client(void)
{
}

static int rtx_alloc_frame(struct tbframe_link *link, u16 len, bool is_ctrl,
			   struct tbframe_frame **frame)
{
	unsigned long flags;
	int i;

	if (len > RTX_FRAME_BYTES)
		return -EINVAL;

	spin_lock_irqsave(&rtx_pool_lock, flags);
	i = find_first_zero_bit(&rtx_pool_used, RTX_POOL);
	if (i >= RTX_POOL) {
		spin_unlock_irqrestore(&rtx_pool_lock, flags);
		return -ENOSPC;
	}
	__set_bit(i, &rtx_pool_used);
	spin_unlock_irqrestore(&rtx_pool_lock, flags);

	rtx_pool_frame[i].data = rtx_pool_buf[i];
	rtx_pool_frame[i].len = len;
	rtx_pool_frame[i].is_ctrl = is_ctrl;
	*frame = &rtx_pool_frame[i];
	return 0;
}

static void rtx_frame_release(struct tbframe_frame *frame)
{
	unsigned long flags;
	int i = frame - rtx_pool_frame;

	if (i < 0 || i >= RTX_POOL)
		return;
	spin_lock_irqsave(&rtx_pool_lock, flags);
	__clear_bit(i, &rtx_pool_used);
	spin_unlock_irqrestore(&rtx_pool_lock, flags);
}

/* The lossy wire: consume every frame, deliver none, timestamp data. */
static int rtx_xmit(struct tbframe_link *link, struct tbframe_frame *frame)
{
	int n;

	if (!frame->is_ctrl) {
		n = atomic_fetch_inc(&rtx_sent);
		if (n < RTX_POOL)
			rtx_sent_at[n] = jiffies;
	}
	rtx_frame_release(frame);
	return 0;
}

static void rtx_frame_free(struct tbframe_link *link,
			   struct tbframe_frame *frame)
{
	rtx_frame_release(frame);
}

static const char *rtx_link_name(const struct tbframe_link *link)
{
	return "rtxmock";
}

static void rtx_link_info(const struct tbframe_link *link,
			  struct tbframe_link_info *info)
{
	memset(info, 0, sizeof(*info));
}

static const struct tbrxe_transport_ops rtx_transport = {
	.register_client	= rtx_register_client,
	.unregister_client	= rtx_unregister_client,
	.alloc_frame		= rtx_alloc_frame,
	.xmit			= rtx_xmit,
	.frame_free		= rtx_frame_free,
	.link_name		= rtx_link_name,
	.link_info		= rtx_link_info,
};

/* Craft an RC ACK for @qpn at @psn and feed it through the rx upcall. */
static void rtx_inject_ack(void *fake_link, u32 qpn, u32 psn)
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

/* Wait for the data-packet xmit count to reach at least @want. */
static int rtx_wait_sent(int want)
{
	int i, cur = 0;

	for (i = 0; i < RTX_POLL_TRIES; i++) {
		cur = atomic_read(&rtx_sent);
		if (cur >= want)
			break;
		msleep(RTX_POLL_MS);
	}
	return cur;
}

static int rtx_connect(struct ib_qp *qp, u32 dest_qpn,
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
	attr.sq_psn = RTX_SQ_PSN;
	attr.timeout = RTX_VERBS_TIMEOUT;
	attr.retry_cnt = 7;
	attr.rnr_retry = 7;
	attr.max_rd_atomic = 1;
	return ib_modify_qp(qp, &attr, IB_QP_STATE | IB_QP_SQ_PSN |
			    IB_QP_TIMEOUT | IB_QP_RETRY_CNT |
			    IB_QP_RNR_RETRY | IB_QP_MAX_QP_RD_ATOMIC);
}

/*
 * A lost packet is re-sent promptly (backoff base, not the verbs curve),
 * and a peer response resets the backoff so the NEXT loss also recovers at
 * the base delay.
 */
static void tbrxe_retransmit_first_delay_bounded(struct kunit *test)
{
	static int fake_link;
	struct ib_cq_init_attr cq_attr = { .cqe = 32 };
	struct ib_qp_init_attr qp_init = {
		.cap = {
			.max_send_wr	= 8,
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
	unsigned long gap;
	u8 *src;
	int sent;

	rtx_pool_used = 0;
	atomic_set(&rtx_sent, 0);
	tbrxe_set_transport_ops(&rtx_transport);
	tbrxe_test_link_up(&fake_link);
	rxe = tbrxe_test_dev(&fake_link);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, rxe);
	dev = &rxe->ib_dev;
	KUNIT_ASSERT_EQ(test, tbrxe_test_gid(rxe, &sgid, &sgid_index), 0);
	tbrxe_test_peer_gid(&peer);

	src = kunit_kzalloc(test, RTX_MSG_LEN, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, src);

	pd = ib_alloc_pd(dev, 0);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, pd);
	cq = ib_create_cq(dev, NULL, NULL, NULL, &cq_attr);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, cq);
	qp_init.send_cq = cq;
	qp_init.recv_cq = cq;
	qp = ib_create_qp(pd, &qp_init);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, qp);

	KUNIT_ASSERT_EQ(test, rtx_connect(qp, 0x42, &peer, sgid_index), 0);

	ssge.addr = (uintptr_t)src;
	ssge.length = RTX_MSG_LEN;
	ssge.lkey = pd->local_dma_lkey;
	swr.wr_id = 1;
	swr.opcode = IB_WR_SEND;
	swr.send_flags = IB_SEND_SIGNALED;
	swr.sg_list = &ssge;
	swr.num_sge = 1;
	KUNIT_ASSERT_EQ(test, ib_post_send(qp, &swr, &bad_swr), 0);

	/* Original transmission, then the wire eats it. */
	sent = rtx_wait_sent(1);
	KUNIT_ASSERT_EQ(test, sent, 1);

	/* The retransmit must arrive promptly, not on the verbs curve. */
	sent = rtx_wait_sent(2);
	KUNIT_ASSERT_GE(test, sent, 2);
	gap = rtx_sent_at[1] - rtx_sent_at[0];
	KUNIT_EXPECT_LT(test, jiffies_to_msecs(gap),
			(unsigned int)RTX_PROMPT_MS);

	/*
	 * Let the loss persist: retransmits 2..4 space out on the doubling
	 * curve (evidence the backoff escalates rather than firing flat).
	 * After four timeouts the un-reset delay would be base << 4 =
	 * 160 ms > RTX_PROMPT_MS, which is what makes the reset assertion
	 * below discriminating.
	 */
	sent = rtx_wait_sent(5);
	KUNIT_ASSERT_GE(test, sent, 5);
	KUNIT_EXPECT_GT(test,
			(unsigned long)(rtx_sent_at[3] - rtx_sent_at[2]),
			(unsigned long)(rtx_sent_at[1] - rtx_sent_at[0]));

	/* Ack it: the WQE retires and the peer response resets the backoff. */
	rtx_inject_ack(&fake_link, qp->qp_num, RTX_SQ_PSN);
	msleep(50);

	/* Second message, lost again: the first retransmit is prompt again
	 * (base delay), not the escalated 160 ms the un-reset shift would
	 * give. This is what keeps the tail flat under repeated loss.
	 */
	sent = atomic_read(&rtx_sent);
	swr.wr_id = 2;
	KUNIT_ASSERT_EQ(test, ib_post_send(qp, &swr, &bad_swr), 0);
	KUNIT_ASSERT_GE(test, rtx_wait_sent(sent + 2), sent + 2);
	gap = rtx_sent_at[sent + 1] - rtx_sent_at[sent];
	KUNIT_EXPECT_LT(test, jiffies_to_msecs(gap),
			(unsigned int)RTX_PROMPT_MS);

	rtx_inject_ack(&fake_link, qp->qp_num, RTX_SQ_PSN + 1);
	msleep(20);

	ib_destroy_qp(qp);
	ib_destroy_cq(cq);
	ib_dealloc_pd(pd);
	tbrxe_test_link_down(&fake_link);
	tbrxe_set_transport_ops(NULL);
}

/*
 * Contract guard: acked progress pushes the retransmit deadline forward.
 * The completer re-arms the retry timer on its empty-queue EXIT path, and
 * a pass that consumes an ACK drains to that EXIT, so the effective timer
 * semantics are gap-since-last-ACK -- a mid-interval partial ACK defers
 * the surviving message's retransmit by a full fresh interval. This case
 * pins that behavior (it is what keeps a slow-but-flowing peer from
 * being rewound); the qps32 bidirectional dup storms on the tbloop rig
 * were NOT a violation of it -- there the real ACK arrival gaps exceed
 * the base interval (ACKs queue behind reverse-direction data), which is
 * an ACK-prioritization problem, not a timer-logic one.
 */
static void tbrxe_retransmit_progress_defers_rewind(struct kunit *test)
{
	static int fake_link;
	struct ib_cq_init_attr cq_attr = { .cqe = 32 };
	struct ib_qp_init_attr qp_init = {
		.cap = {
			.max_send_wr	= 8,
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
	uint saved_base = tbrxe_retransmit_base_ms;
	unsigned long progress_at, gap;
	struct ib_pd *pd;
	struct ib_cq *cq;
	struct ib_qp *qp;
	u8 *src;
	int sent;

	/* Wide interval so the 1x-vs-2x deadline discrimination survives
	 * qemu scheduling noise.
	 */
	tbrxe_retransmit_base_ms = 200;

	rtx_pool_used = 0;
	atomic_set(&rtx_sent, 0);
	tbrxe_set_transport_ops(&rtx_transport);
	tbrxe_test_link_up(&fake_link);
	rxe = tbrxe_test_dev(&fake_link);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, rxe);
	dev = &rxe->ib_dev;
	KUNIT_ASSERT_EQ(test, tbrxe_test_gid(rxe, &sgid, &sgid_index), 0);
	tbrxe_test_peer_gid(&peer);

	src = kunit_kzalloc(test, RTX_MSG_LEN, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, src);

	pd = ib_alloc_pd(dev, 0);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, pd);
	cq = ib_create_cq(dev, NULL, NULL, NULL, &cq_attr);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, cq);
	qp_init.send_cq = cq;
	qp_init.recv_cq = cq;
	qp = ib_create_qp(pd, &qp_init);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, qp);

	KUNIT_ASSERT_EQ(test, rtx_connect(qp, 0x42, &peer, sgid_index), 0);

	ssge.addr = (uintptr_t)src;
	ssge.length = RTX_MSG_LEN;
	ssge.lkey = pd->local_dma_lkey;
	swr.opcode = IB_WR_SEND;
	swr.send_flags = IB_SEND_SIGNALED;
	swr.sg_list = &ssge;
	swr.num_sge = 1;

	/* A then B, one packet each; the wire eats both. */
	swr.wr_id = 1;
	KUNIT_ASSERT_EQ(test, ib_post_send(qp, &swr, &bad_swr), 0);
	swr.wr_id = 2;
	KUNIT_ASSERT_EQ(test, ib_post_send(qp, &swr, &bad_swr), 0);
	KUNIT_ASSERT_GE(test, rtx_wait_sent(2), 2);

	/* Mid-interval, A is acked: real progress while B waits. */
	msleep(tbrxe_retransmit_base_ms / 2);
	rtx_inject_ack(&fake_link, qp->qp_num, RTX_SQ_PSN);
	progress_at = jiffies;

	/* B's retransmit: a full fresh interval after the progress. */
	KUNIT_ASSERT_GE(test, rtx_wait_sent(3), 3);
	gap = rtx_sent_at[2] - progress_at;
	KUNIT_EXPECT_GE(test, jiffies_to_msecs(gap),
			(unsigned int)(tbrxe_retransmit_base_ms * 3 / 4));

	rtx_inject_ack(&fake_link, qp->qp_num, RTX_SQ_PSN + 1);
	msleep(20);

	ib_destroy_qp(qp);
	ib_destroy_cq(cq);
	ib_dealloc_pd(pd);
	tbrxe_test_link_down(&fake_link);
	tbrxe_set_transport_ops(NULL);
	tbrxe_retransmit_base_ms = saved_base;
}

/*
 * The delay curve itself: base << shift, capped at the verbs timeout;
 * base 0 disables (stock verbs interval); the delay is never 0 when a
 * verbs timeout exists.
 */
static void tbrxe_retransmit_backoff_curve(struct kunit *test)
{
	unsigned long t = msecs_to_jiffies(500);
	unsigned long base = msecs_to_jiffies(10);

	KUNIT_EXPECT_EQ(test, rxe_retrans_backoff_jiffies(t, base, 0), base);
	KUNIT_EXPECT_EQ(test, rxe_retrans_backoff_jiffies(t, base, 1),
			2 * base);
	KUNIT_EXPECT_EQ(test, rxe_retrans_backoff_jiffies(t, base, 3),
			8 * base);
	/* Caps at the verbs timeout. */
	KUNIT_EXPECT_EQ(test, rxe_retrans_backoff_jiffies(t, base, 10), t);
	/* Enormous shifts do not wrap. */
	KUNIT_EXPECT_EQ(test, rxe_retrans_backoff_jiffies(t, base, 200), t);
	/* base 0 = stock behavior. */
	KUNIT_EXPECT_EQ(test, rxe_retrans_backoff_jiffies(t, 0, 0), t);
	/* Never 0 when a verbs timeout exists. */
	KUNIT_EXPECT_GE(test, rxe_retrans_backoff_jiffies(1, 1, 0), 1ul);
}

static struct kunit_case tbrxe_retransmit_cases[] = {
	KUNIT_CASE(tbrxe_retransmit_first_delay_bounded),
	KUNIT_CASE(tbrxe_retransmit_progress_defers_rewind),
	KUNIT_CASE(tbrxe_retransmit_backoff_curve),
	{}
};

static struct kunit_suite tbrxe_retransmit_suite = {
	.name = "tbrxe_retransmit",
	.test_cases = tbrxe_retransmit_cases,
};

kunit_test_suites(&tbrxe_retransmit_suite);
