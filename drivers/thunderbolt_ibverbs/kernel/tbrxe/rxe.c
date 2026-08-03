// SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB
/*
 * Copyright (c) 2016 Mellanox Technologies Ltd. All rights reserved.
 * Copyright (c) 2015 System Fabric Works, Inc. All rights reserved.
 */

#include <net/addrconf.h>
#include "rxe.h"
#include "rxe_loc.h"
#include "tbrxe_frame.h"

MODULE_AUTHOR("Bob Pearson, Frank Zago, John Groves, Kamal Heib");
MODULE_DESCRIPTION("rxe-derived RDMA engine over the tbframe Thunderbolt frame service");
MODULE_LICENSE("Dual BSD/GPL");
#ifdef TBV_PKG_VERSION
MODULE_VERSION(TBV_PKG_VERSION);
#endif

/* The single scaffold device (see tbrxe_frame.c on the one-port choice). */
static struct rxe_dev *tbrxe_dev;
static bool tbrxe_dev_published;

struct rxe_dev *tbrxe_get_dev(void)
{
	return tbrxe_dev;
}

/* free resources for a rxe device all objects created for this device must
 * have been destroyed
 */
void rxe_dealloc(struct ib_device *ib_dev)
{
	struct rxe_dev *rxe = container_of(ib_dev, struct rxe_dev, ib_dev);

	rxe_pool_cleanup(&rxe->uc_pool);
	rxe_pool_cleanup(&rxe->pd_pool);
	rxe_pool_cleanup(&rxe->ah_pool);
	rxe_pool_cleanup(&rxe->srq_pool);
	rxe_pool_cleanup(&rxe->qp_pool);
	rxe_pool_cleanup(&rxe->cq_pool);
	rxe_pool_cleanup(&rxe->mr_pool);
	rxe_pool_cleanup(&rxe->mw_pool);

	WARN_ON(!RB_EMPTY_ROOT(&rxe->mcg_tree));

	mutex_destroy(&rxe->usdev_lock);
}

static const struct ib_device_ops rxe_ib_dev_odp_ops = {
	.advise_mr = rxe_ib_advise_mr,
};

/* initialize rxe device parameters */
static void rxe_init_device_param(struct rxe_dev *rxe)
{
	rxe->max_inline_data			= RXE_MAX_INLINE_DATA;

	rxe->attr.vendor_id			= RXE_VENDOR_ID;
	rxe->attr.max_mr_size			= RXE_MAX_MR_SIZE;
	rxe->attr.page_size_cap			= RXE_PAGE_SIZE_CAP;
	rxe->attr.max_qp			= RXE_MAX_QP;
	rxe->attr.max_qp_wr			= RXE_MAX_QP_WR;
	rxe->attr.device_cap_flags		= RXE_DEVICE_CAP_FLAGS;
	/*
	 * AppMana uses RXE as the always-present NCCL fallback rail
	 * (rxe_lan). User-initiated RDMA_NLDEV_CMD_DELLINK on a live
	 * rxe_lan can force unregister while NCCL/Ray still owns QPs, which
	 * has wedged hosts in ib_unregister_device teardown. Keep netdev
	 * unregister/module unload cleanup, but make "rdma link del" fail
	 * fast instead of tearing down live transport.
	 */
	rxe->attr.kernel_cap_flags		= 0;
	rxe->attr.max_send_sge			= RXE_MAX_SGE;
	rxe->attr.max_recv_sge			= RXE_MAX_SGE;
	rxe->attr.max_sge_rd			= RXE_MAX_SGE_RD;
	rxe->attr.max_cq			= RXE_MAX_CQ;
	rxe->attr.max_cqe			= (1 << RXE_MAX_LOG_CQE) - 1;
	rxe->attr.max_mr			= RXE_MAX_MR;
	rxe->attr.max_mw			= RXE_MAX_MW;
	rxe->attr.max_pd			= RXE_MAX_PD;
	rxe->attr.max_qp_rd_atom		= RXE_MAX_QP_RD_ATOM;
	rxe->attr.max_res_rd_atom		= RXE_MAX_RES_RD_ATOM;
	rxe->attr.max_qp_init_rd_atom		= RXE_MAX_QP_INIT_RD_ATOM;
	rxe->attr.atomic_cap			= IB_ATOMIC_HCA;
	rxe->attr.max_mcast_grp			= RXE_MAX_MCAST_GRP;
	rxe->attr.max_mcast_qp_attach		= RXE_MAX_MCAST_QP_ATTACH;
	rxe->attr.max_total_mcast_qp_attach	= RXE_MAX_TOT_MCAST_QP_ATTACH;
	rxe->attr.max_ah			= RXE_MAX_AH;
	rxe->attr.max_srq			= RXE_MAX_SRQ;
	rxe->attr.max_srq_wr			= RXE_MAX_SRQ_WR;
	rxe->attr.max_srq_sge			= RXE_MAX_SRQ_SGE;
	rxe->attr.max_fast_reg_page_list_len	= RXE_MAX_FMR_PAGE_LIST_LEN;
	rxe->attr.max_pkeys			= RXE_MAX_PKEYS;
	rxe->attr.local_ca_ack_delay		= RXE_LOCAL_CA_ACK_DELAY;

	/*
	 * There is no netdev underneath a tbrxe device. A per-boot random
	 * address seeds the node/sys-image GUIDs only; the self GID comes
	 * from the tbframe-advertised identity at the first link_up
	 * (tbrxe_client_link_up), never from this address.
	 */
	eth_random_addr(rxe->raw_gid);

	addrconf_addr_eui48((unsigned char *)&rxe->attr.sys_image_guid,
			rxe->raw_gid);

	rxe->max_ucontext			= RXE_MAX_UCONTEXT;

	if (IS_ENABLED(CONFIG_INFINIBAND_ON_DEMAND_PAGING)) {
		rxe->attr.kernel_cap_flags |= IBK_ON_DEMAND_PAGING;

		/* IB_ODP_SUPPORT_IMPLICIT is not supported right now. */
		rxe->attr.odp_caps.general_caps |= IB_ODP_SUPPORT;

		rxe->attr.odp_caps.per_transport_caps.ud_odp_caps |= IB_ODP_SUPPORT_SEND;
		rxe->attr.odp_caps.per_transport_caps.ud_odp_caps |= IB_ODP_SUPPORT_RECV;
		rxe->attr.odp_caps.per_transport_caps.ud_odp_caps |= IB_ODP_SUPPORT_SRQ_RECV;

		rxe->attr.odp_caps.per_transport_caps.rc_odp_caps |= IB_ODP_SUPPORT_SEND;
		rxe->attr.odp_caps.per_transport_caps.rc_odp_caps |= IB_ODP_SUPPORT_RECV;
		rxe->attr.odp_caps.per_transport_caps.rc_odp_caps |= IB_ODP_SUPPORT_WRITE;
		rxe->attr.odp_caps.per_transport_caps.rc_odp_caps |= IB_ODP_SUPPORT_READ;
		rxe->attr.odp_caps.per_transport_caps.rc_odp_caps |= IB_ODP_SUPPORT_ATOMIC;
		rxe->attr.odp_caps.per_transport_caps.rc_odp_caps |= IB_ODP_SUPPORT_SRQ_RECV;
		rxe->attr.odp_caps.per_transport_caps.rc_odp_caps |= IB_ODP_SUPPORT_FLUSH;
		rxe->attr.odp_caps.per_transport_caps.rc_odp_caps |= IB_ODP_SUPPORT_ATOMIC_WRITE;

		/* set handler for ODP prefetching API - ibv_advise_mr(3) */
		ib_set_device_ops(&rxe->ib_dev, &rxe_ib_dev_odp_ops);
	}
}

/* initialize port attributes */
static void rxe_init_port_param(struct rxe_port *port)
{
	/*
	 * The port is reported ACTIVE from the start: the loopback path is
	 * always usable without hardware. Link transitions still dispatch
	 * IB_EVENT_PORT_ACTIVE/PORT_ERR (tbrxe_frame.c).
	 */
	port->attr.state		= IB_PORT_ACTIVE;
	/* Strict IB_MTU_2048 per tbframe-tbrxe-wire-spec.md section 5. */
	port->attr.max_mtu		= IB_MTU_2048;
	port->attr.active_mtu		= IB_MTU_2048;
	port->attr.gid_tbl_len		= RXE_PORT_GID_TBL_LEN;
	port->attr.port_cap_flags	= RXE_PORT_PORT_CAP_FLAGS;
	port->attr.max_msg_sz		= RXE_PORT_MAX_MSG_SZ;
	port->attr.bad_pkey_cntr	= RXE_PORT_BAD_PKEY_CNTR;
	port->attr.qkey_viol_cntr	= RXE_PORT_QKEY_VIOL_CNTR;
	port->attr.pkey_tbl_len		= RXE_PORT_PKEY_TBL_LEN;
	port->attr.lid			= RXE_PORT_LID;
	port->attr.sm_lid		= RXE_PORT_SM_LID;
	port->attr.lmc			= RXE_PORT_LMC;
	port->attr.max_vl_num		= RXE_PORT_MAX_VL_NUM;
	port->attr.sm_sl		= RXE_PORT_SM_SL;
	port->attr.subnet_timeout	= RXE_PORT_SUBNET_TIMEOUT;
	port->attr.init_type_reply	= RXE_PORT_INIT_TYPE_REPLY;
	port->attr.active_width		= RXE_PORT_ACTIVE_WIDTH;
	port->attr.active_speed		= RXE_PORT_ACTIVE_SPEED;
	port->attr.phys_state		= IB_PORT_PHYS_STATE_LINK_UP;
	port->mtu_cap			= ib_mtu_enum_to_int(IB_MTU_2048);
	port->subnet_prefix		= cpu_to_be64(RXE_PORT_SUBNET_PREFIX);
}

/* initialize port state, note IB convention that HCA ports are always
 * numbered from 1
 */
static void rxe_init_ports(struct rxe_dev *rxe)
{
	struct rxe_port *port = &rxe->port;

	rxe_init_port_param(port);
	addrconf_addr_eui48((unsigned char *)&port->port_guid,
			    rxe->raw_gid);
	spin_lock_init(&port->port_lock);
}

/* init pools of managed objects */
static void rxe_init_pools(struct rxe_dev *rxe)
{
	rxe_pool_init(rxe, &rxe->uc_pool, RXE_TYPE_UC);
	rxe_pool_init(rxe, &rxe->pd_pool, RXE_TYPE_PD);
	rxe_pool_init(rxe, &rxe->ah_pool, RXE_TYPE_AH);
	rxe_pool_init(rxe, &rxe->srq_pool, RXE_TYPE_SRQ);
	rxe_pool_init(rxe, &rxe->qp_pool, RXE_TYPE_QP);
	rxe_pool_init(rxe, &rxe->cq_pool, RXE_TYPE_CQ);
	rxe_pool_init(rxe, &rxe->mr_pool, RXE_TYPE_MR);
	rxe_pool_init(rxe, &rxe->mw_pool, RXE_TYPE_MW);
}

/* initialize rxe device state */
static void rxe_init(struct rxe_dev *rxe)
{
	/* init default device parameters */
	rxe_init_device_param(rxe);

	rxe_init_ports(rxe);
	rxe_init_pools(rxe);

	/* init pending mmap list */
	spin_lock_init(&rxe->mmap_offset_lock);
	spin_lock_init(&rxe->pending_lock);
	INIT_LIST_HEAD(&rxe->pending_mmaps);

	/* init multicast support */
	spin_lock_init(&rxe->mcg_lock);
	rxe->mcg_tree = RB_ROOT;

	mutex_init(&rxe->usdev_lock);
}

void rxe_set_mtu(struct rxe_dev *rxe, unsigned int frame_payload)
{
	struct rxe_port *port = &rxe->port;
	enum ib_mtu mtu;

	mtu = eth_mtu_int_to_enum(frame_payload);

	/*
	 * Strict IB_MTU_2048 ceiling (wire-spec section 5): worst-case
	 * transport unit 80 + 2048 + pad + 4 always fits the 4096-byte
	 * tbframe frame; links with a smaller max_payload are rejected at
	 * link_up (tbrxe_frame.c).
	 */
	mtu = mtu ? min_t(enum ib_mtu, mtu, IB_MTU_2048) : IB_MTU_256;

	port->attr.active_mtu = mtu;
	port->mtu_cap = ib_mtu_enum_to_int(mtu);
}

/* Create a tbrxe device. The caller should allocate memory for rxe by
 * calling ib_alloc_device. Publication (ib_register_device) is deferred to
 * the first tbframe link_up: the self GID is the identity tbframe
 * advertised in our own HELLO, which does not exist before a link is up.
 */
int rxe_add(struct rxe_dev *rxe, unsigned int mtu)
{
	rxe_init(rxe);
	rxe_set_mtu(rxe, mtu);
	rxe_init_device(rxe);
	tbrxe_frame_init(rxe);

	return 0;
}

/* First-link publication (called from tbrxe_client_link_up with the self
 * GID already set, so the registration-time GID cache fill sees it).
 */
int tbrxe_publish(struct rxe_dev *rxe)
{
	int err;

	if (WARN_ON(rxe != tbrxe_dev || tbrxe_dev_published))
		return -EINVAL;

	err = rxe_register_device(rxe, "tbrxe%d");
	if (err)
		return err;

	tbrxe_dev_published = true;
	dev_info(&rxe->ib_dev.dev, "published over tbframe\n");
	return 0;
}

static int __init rxe_module_init(void)
{
	struct rxe_dev *rxe;
	int err;

	err = rxe_alloc_wq();
	if (err)
		return err;

	rxe = ib_alloc_device(rxe_dev, ib_dev);
	if (!rxe) {
		rxe_destroy_wq();
		return -ENOMEM;
	}

	/* Frame payload budget: eth_mtu_int_to_enum() subtracts the 80-byte
	 * header allowance, so hand it 2048 + 80 to land on IB_MTU_2048.
	 */
	err = rxe_add(rxe, 2048 + RXE_MAX_HDR_LENGTH);
	if (err) {
		ib_dealloc_device(&rxe->ib_dev);
		rxe_destroy_wq();
		return err;
	}
	tbrxe_dev = rxe;

	err = tbrxe_frame_register(rxe);
	if (err) {
		/* Never published (no link_up can have run): dealloc, not
		 * unregister; dealloc_driver is wired by rxe_init_device().
		 */
		ib_dealloc_device(&rxe->ib_dev);
		tbrxe_dev = NULL;
		rxe_destroy_wq();
		return err;
	}

	pr_info("loaded\n");
	return 0;
}

static void __exit rxe_module_exit(void)
{
	tbrxe_frame_unregister();
	/*
	 * NOT ib_unregister_driver(RDMA_DRIVER_RXE): the driver id is shared
	 * with the stock rdma_rxe module (so stock librxe binds to us), and
	 * unregister-by-driver-id would tear down live rdma_rxe devices
	 * (rxe_lan) too.
	 */
	if (tbrxe_dev) {
		if (tbrxe_dev_published)
			ib_unregister_device(&tbrxe_dev->ib_dev);
		else
			ib_dealloc_device(&tbrxe_dev->ib_dev);
		tbrxe_dev = NULL;
		tbrxe_dev_published = false;
	}
	rxe_destroy_wq();

	pr_info("unloaded\n");
}

late_initcall(rxe_module_init);
module_exit(rxe_module_exit);
