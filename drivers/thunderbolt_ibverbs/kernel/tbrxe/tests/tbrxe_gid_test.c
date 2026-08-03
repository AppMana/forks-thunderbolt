// SPDX-License-Identifier: GPL-2.0
/*
 * tbrxe_gid_test.c - regression: the GID identity split that hung the
 * first two-node traffic (appmana-023/025, "Failed allocating skb").
 *
 * Peers exchange tbrxe's SELF GID out of band (perftest bootstrap), but the
 * requester's link lookup matches the dgid against peer GIDs derived from
 * the tbframe HELLO gid_eui64. If the self GID is derived from any identity
 * OTHER than the one tbframe advertised in our own HELLO
 * (tbframe_link_info.local_gid_eui64), the two derivations disagree, the
 * dgid never matches any link, rxe_init_packet() returns NULL and the
 * requester parks forever.
 *
 * The contract here: tbrxe_query_gid(index 0) must equal
 * tbrxe_gid_from_eui64(local_gid_eui64) - exactly what the peer derives
 * from our HELLO - and a requester alloc for a dgid derived from the peer's
 * HELLO must find the link and return a packet.
 */

#include <kunit/test.h>
#include <rdma/ib_verbs.h>

#include "rxe.h"
#include "rxe_loc.h"
#include "tbrxe_test_link.h"

static u8 gid_mock_buf[TBFRAME_MAX_FRAME];
static struct tbframe_frame gid_mock_frame;
static atomic_t gid_mock_allocs;

static int gid_mock_register_client(const struct tbframe_client_ops *ops,
				    void *ctx)
{
	return 0;
}

static void gid_mock_unregister_client(void)
{
}

static int gid_mock_alloc_frame(struct tbframe_link *link, u16 len,
				bool is_ctrl, struct tbframe_frame **frame)
{
	atomic_inc(&gid_mock_allocs);
	gid_mock_frame.data = gid_mock_buf;
	gid_mock_frame.len = len;
	*frame = &gid_mock_frame;
	return 0;
}

static int gid_mock_xmit(struct tbframe_link *link,
			 struct tbframe_frame *frame)
{
	return 0;
}

static void gid_mock_frame_free(struct tbframe_link *link,
				struct tbframe_frame *frame)
{
}

static const char *gid_mock_link_name(const struct tbframe_link *link)
{
	return "gidmock";
}

static void gid_mock_link_info(const struct tbframe_link *link,
			       struct tbframe_link_info *info)
{
	memset(info, 0, sizeof(*info));
}

static const struct tbrxe_transport_ops gid_mock_transport = {
	.register_client	= gid_mock_register_client,
	.unregister_client	= gid_mock_unregister_client,
	.alloc_frame		= gid_mock_alloc_frame,
	.xmit			= gid_mock_xmit,
	.frame_free		= gid_mock_frame_free,
	.link_name		= gid_mock_link_name,
	.link_info		= gid_mock_link_info,
};

static void tbrxe_gid_identity_matches_hello(struct kunit *test)
{
	static int fake_link;
	union ib_gid self, expect, peer;
	struct rxe_pkt_info pkt = {};
	struct rxe_av av = {};
	struct rxe_dev *rxe;
	struct sk_buff *skb;

	rxe = tbrxe_get_dev();
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, rxe);

	atomic_set(&gid_mock_allocs, 0);
	tbrxe_set_transport_ops(&gid_mock_transport);
	tbrxe_test_link_up(&fake_link);

	/* End-to-end agreement: the GID we publish at index 0 (what
	 * userspace hands the peer through the perftest bootstrap) must be
	 * the GID the peer derives from OUR HELLO.
	 */
	KUNIT_ASSERT_EQ(test, tbrxe_query_gid(rxe, 0, &self), 0);
	tbrxe_gid_from_eui64(TBRXE_TEST_LOCAL_EUI64, &expect);
	KUNIT_EXPECT_MEMEQ(test, self.raw, expect.raw, sizeof(self.raw));

	/* Requester alloc for a dgid equal to what the peer's HELLO
	 * advertised: the link lookup must hit and produce a packet, not
	 * NULL ("Failed allocating skb" park).
	 */
	tbrxe_gid_from_eui64(TBRXE_TEST_PEER_EUI64, &peer);
	memcpy(&av.grh.dgid, &peer, sizeof(peer));
	skb = rxe_init_packet(rxe, &av, 128, &pkt);
	KUNIT_EXPECT_NOT_ERR_OR_NULL(test, skb);
	KUNIT_EXPECT_EQ(test, 1, atomic_read(&gid_mock_allocs));
	if (!IS_ERR_OR_NULL(skb))
		kfree_skb(skb);

	tbrxe_test_link_down(&fake_link);
	tbrxe_set_transport_ops(NULL);
}

/*
 * Regression: a GRH-less AV (perftest without -x on this IB link-layer
 * port) must be rejected at modify_qp, not crash. tbrxe is GID-addressed
 * only; rxe_av_fill_ip_info used to deref the NULL sgid_attr (appmana-023
 * kdump 202608031446, NULL at rdma_gid2ip+0x0).
 */
static void tbrxe_grhless_av_is_rejected(struct kunit *test)
{
	static int fake_link;
	struct ib_cq_init_attr cq_attr = { .cqe = 4 };
	struct ib_qp_init_attr qp_init = {
		.cap = {
			.max_send_wr	= 2,
			.max_recv_wr	= 2,
			.max_send_sge	= 1,
			.max_recv_sge	= 1,
		},
		.sq_sig_type	= IB_SIGNAL_ALL_WR,
		.qp_type	= IB_QPT_RC,
	};
	struct ib_qp_attr attr;
	struct rxe_dev *rxe;
	struct ib_device *dev;
	struct ib_pd *pd;
	struct ib_cq *cq;
	struct ib_qp *qp;
	int err;

	rxe = tbrxe_get_dev();
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, rxe);

	tbrxe_set_transport_ops(&gid_mock_transport);
	tbrxe_test_link_up(&fake_link);
	dev = &rxe->ib_dev;

	pd = ib_alloc_pd(dev, 0);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, pd);
	cq = ib_create_cq(dev, NULL, NULL, NULL, &cq_attr);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, cq);
	qp_init.send_cq = cq;
	qp_init.recv_cq = cq;
	qp = ib_create_qp(pd, &qp_init);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, qp);

	memset(&attr, 0, sizeof(attr));
	attr.qp_state = IB_QPS_INIT;
	attr.pkey_index = 0;
	attr.port_num = 1;
	attr.qp_access_flags = IB_ACCESS_LOCAL_WRITE;
	KUNIT_ASSERT_EQ(test, ib_modify_qp(qp, &attr,
					   IB_QP_STATE | IB_QP_PKEY_INDEX |
					   IB_QP_PORT | IB_QP_ACCESS_FLAGS), 0);

	/* RTR with an AV that carries no GRH: must fail, not oops. */
	memset(&attr, 0, sizeof(attr));
	attr.qp_state = IB_QPS_RTR;
	attr.path_mtu = IB_MTU_2048;
	attr.dest_qp_num = qp->qp_num;
	attr.rq_psn = 1;
	attr.max_dest_rd_atomic = 1;
	attr.min_rnr_timer = 12;
	attr.ah_attr.type = rdma_ah_find_type(dev, 1);
	rdma_ah_set_port_num(&attr.ah_attr, 1);
	rdma_ah_set_dlid(&attr.ah_attr, 0);
	err = ib_modify_qp(qp, &attr, IB_QP_STATE | IB_QP_AV |
			   IB_QP_PATH_MTU | IB_QP_DEST_QPN | IB_QP_RQ_PSN |
			   IB_QP_MAX_DEST_RD_ATOMIC | IB_QP_MIN_RNR_TIMER);
	KUNIT_EXPECT_NE(test, err, 0);

	ib_destroy_qp(qp);
	ib_destroy_cq(cq);
	ib_dealloc_pd(pd);
	tbrxe_test_link_down(&fake_link);
	tbrxe_set_transport_ops(NULL);
}

static struct kunit_case tbrxe_gid_cases[] = {
	KUNIT_CASE(tbrxe_gid_identity_matches_hello),
	KUNIT_CASE(tbrxe_grhless_av_is_rejected),
	{}
};

static struct kunit_suite tbrxe_gid_suite = {
	.name = "tbrxe_gid",
	.test_cases = tbrxe_gid_cases,
};

kunit_test_suites(&tbrxe_gid_suite);
