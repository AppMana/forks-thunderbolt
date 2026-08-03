// SPDX-License-Identifier: GPL-2.0
/*
 * tbrxe_perlink_test.c - the per-link ib_device + GID-anchor netdev model
 * (wire-spec section 8, normative).
 *
 * One ib_device per tbframe link, published at that link's first link_up
 * and unpublished on terminal link_down; each device bound to an IFF_NOARP
 * netdev whose xmit drops, from whose addresses ib_core populates the RoCE
 * GID table. The driver carries no GID identity of its own.
 */

#include <kunit/test.h>
#include <rdma/ib_verbs.h>

#include "rxe.h"
#include "tbrxe_test_link.h"

static int perlink_register_client(const struct tbframe_client_ops *ops,
				   void *ctx)
{
	return 0;
}

static void perlink_unregister_client(void)
{
}

static int perlink_alloc_frame(struct tbframe_link *link, u16 len,
			       bool is_ctrl, struct tbframe_frame **frame)
{
	return -ENETDOWN;
}

static int perlink_xmit(struct tbframe_link *link, struct tbframe_frame *frame)
{
	return -ENETDOWN;
}

static void perlink_frame_free(struct tbframe_link *link,
			       struct tbframe_frame *frame)
{
}

static const char *perlink_link_name(const struct tbframe_link *link)
{
	return "perlinkmock";
}

static void perlink_link_info(const struct tbframe_link *link,
			      struct tbframe_link_info *info)
{
	memset(info, 0, sizeof(*info));
}

static const struct tbrxe_transport_ops perlink_transport = {
	.register_client	= perlink_register_client,
	.unregister_client	= perlink_unregister_client,
	.alloc_frame		= perlink_alloc_frame,
	.xmit			= perlink_xmit,
	.frame_free		= perlink_frame_free,
	.link_name		= perlink_link_name,
	.link_info		= perlink_link_info,
};

static void perlink_link_up_id(void *fake_link, u64 local_eui64,
			       u64 peer_eui64, const char *peer_name)
{
	struct tbframe_link_info info = {
		.gid_eui64	= peer_eui64,
		.local_gid_eui64 = local_eui64,
		.rx_ring_entries = 2048,
		.data_window	= 1984,
		.max_payload	= TBFRAME_MAX_FRAME,
		.width		= 1,
		.speed		= 20,
	};

	strscpy(info.remote_name, peer_name, sizeof(info.remote_name));
	tbrxe_frame_client_ops()->link_up(NULL, fake_link, &info);
}

/*
 * Two links get two devices, each usb4_rdma*-named and bound to its own
 * u4r* GID-anchor netdev (IFF_NOARP, identity MAC); a terminal link_down
 * unpublishes exactly that link's device.
 */
static void tbrxe_perlink_device_per_link(struct kunit *test)
{
	static int fake_a, fake_b;
	struct rxe_dev *rxe_a, *rxe_b;
	struct net_device *ndev;
	u8 mac[ETH_ALEN];

	tbrxe_set_transport_ops(&perlink_transport);

	perlink_link_up_id(&fake_a, TBRXE_TEST_LOCAL_EUI64,
			   TBRXE_TEST_PEER_EUI64, "peer-a");
	perlink_link_up_id(&fake_b, 0x0002d3fffe112233ull,
			   0x00445566fffe7788ull, "peer-b");

	rxe_a = tbrxe_test_dev(&fake_a);
	rxe_b = tbrxe_test_dev(&fake_b);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, rxe_a);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, rxe_b);
	KUNIT_EXPECT_PTR_NE(test, rxe_a, rxe_b);

	/* usb4_rdma*-compatible device naming (webhook HCA patterns). */
	KUNIT_EXPECT_TRUE(test,
			  strstarts(dev_name(&rxe_a->ib_dev.dev), "usb4_rdma"));
	KUNIT_EXPECT_TRUE(test,
			  strstarts(dev_name(&rxe_b->ib_dev.dev), "usb4_rdma"));

	/* Each device is bound to its own GID-anchor netdev: u4r* kernel
	 * name (the udev helpers rename to tbr-<peer>), NOARP, and the MAC
	 * behind the advertised local EUI-64 identity.
	 */
	ndev = ib_device_get_netdev(&rxe_a->ib_dev, 1);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ndev);
	KUNIT_EXPECT_TRUE(test, strstarts(netdev_name(ndev), "u4r"));
	KUNIT_EXPECT_TRUE(test, !!(ndev->flags & IFF_NOARP));
	mac[0] = (u8)((TBRXE_TEST_LOCAL_EUI64 >> 56) & 0xff) ^ 0x02;
	mac[1] = (u8)(TBRXE_TEST_LOCAL_EUI64 >> 48);
	mac[2] = (u8)(TBRXE_TEST_LOCAL_EUI64 >> 40);
	mac[3] = (u8)(TBRXE_TEST_LOCAL_EUI64 >> 16);
	mac[4] = (u8)(TBRXE_TEST_LOCAL_EUI64 >> 8);
	mac[5] = (u8)TBRXE_TEST_LOCAL_EUI64;
	KUNIT_EXPECT_MEMEQ(test, ndev->dev_addr, mac, ETH_ALEN);
	dev_put(ndev);

	/* Terminal down unpublishes b only; a survives. */
	tbrxe_test_link_down(&fake_b);
	KUNIT_EXPECT_PTR_EQ(test, tbrxe_test_dev(&fake_b),
			    (struct rxe_dev *)NULL);
	KUNIT_EXPECT_PTR_EQ(test, tbrxe_test_dev(&fake_a), rxe_a);

	tbrxe_test_link_down(&fake_a);
	KUNIT_EXPECT_PTR_EQ(test, tbrxe_test_dev(&fake_a),
			    (struct rxe_dev *)NULL);

	tbrxe_set_transport_ops(NULL);
}

/*
 * ib_core populates the RoCE GID table from the anchor netdev's addresses:
 * bringing the netdev up creates the identity link-local, and the GID
 * appears without any driver GID code.
 */
static void tbrxe_perlink_gid_from_netdev(struct kunit *test)
{
	static int fake_link;
	struct rxe_dev *rxe;
	union ib_gid gid;
	u8 gid_index = 0;

	tbrxe_set_transport_ops(&perlink_transport);
	tbrxe_test_link_up(&fake_link);
	rxe = tbrxe_test_dev(&fake_link);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, rxe);

	KUNIT_EXPECT_EQ(test, tbrxe_test_gid(rxe, &gid, &gid_index), 0);

	tbrxe_test_link_down(&fake_link);
	tbrxe_set_transport_ops(NULL);
}

/*
 * Regression (kdump 202608031446): a GRH-less AV must be rejected at
 * modify_qp, not crash. On the RoCE port the core's ah checks enforce it.
 */
static void tbrxe_perlink_grhless_av_is_rejected(struct kunit *test)
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

	tbrxe_set_transport_ops(&perlink_transport);
	tbrxe_test_link_up(&fake_link);
	rxe = tbrxe_test_dev(&fake_link);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, rxe);
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

static struct kunit_case tbrxe_perlink_cases[] = {
	KUNIT_CASE(tbrxe_perlink_device_per_link),
	KUNIT_CASE(tbrxe_perlink_gid_from_netdev),
	KUNIT_CASE(tbrxe_perlink_grhless_av_is_rejected),
	{}
};

static struct kunit_suite tbrxe_perlink_suite = {
	.name = "tbrxe_perlink",
	.test_cases = tbrxe_perlink_cases,
};

kunit_test_suites(&tbrxe_perlink_suite);
