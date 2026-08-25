/* SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB */
/*
 * Copyright (c) 2016 Mellanox Technologies Ltd. All rights reserved.
 * Copyright (c) 2015 System Fabric Works, Inc. All rights reserved.
 */

#ifndef RXE_H
#define RXE_H

#ifdef pr_fmt
#undef pr_fmt
#endif
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/skbuff.h>

#include <rdma/ib_verbs.h>
#include <rdma/ib_user_verbs.h>
#include <rdma/ib_pack.h>
#include <rdma/ib_smi.h>
#include <rdma/ib_umem.h>
#include <rdma/ib_cache.h>
#include <rdma/ib_addr.h>

#include "rxe_opcode.h"
#include "rxe_hdr.h"
#include "rxe_param.h"
#include "rxe_verbs.h"
#include "rxe_loc.h"

/*
 * Version 1 and Version 2 are identical on 64 bit machines, but on 32 bit
 * machines Version 2 has a different struct layout.
 */
#define RXE_UVERBS_ABI_VERSION		2

/*
 * tbrxe's own RDMA driver id.
 *
 * The driver id is the unregistration namespace, not just a label:
 * ib_unregister_driver() (drivers/infiniband/core/device.c:1639) sweeps every
 * device in the global xarray whose ops.driver_id matches, with no owner or
 * module check. While tbrxe inherited RDMA_DRIVER_RXE from the rxe fork,
 * `rmmod rdma_rxe` -- whose exit path is ib_unregister_driver(RDMA_DRIVER_RXE)
 * -- tore down every live tbrxe device on the node, and symmetrically tbrxe
 * could not use that fence for its own module exit without destroying the
 * rxe_lan fallback rail.
 *
 * enum rdma_driver_id (include/uapi/rdma/ib_user_ioctl_verbs.h:235) has no
 * value for an out-of-tree USB4 provider, and RDMA_DRIVER_UNKNOWN is not a
 * private namespace: ib_core documents and implements it as "matches all" in
 * device lookup (device.c:367-381, :2364-2381) and rdma-core skips driver-id
 * matching entirely when the kernel reports it (libibverbs/init.c:326, :448).
 * Claiming the next free enum slot would collide with whatever upstream
 * allocates there next, so use a value far outside the allocated range.
 *
 * Kernel side this is only ever compared for equality (device.c:379, :1646,
 * :2381, uverbs_ioctl.c:570) and exported to userspace as a u32 netlink
 * attribute (RDMA_NLDEV_ATTR_UVERBS_DRIVER_ID, uverbs_main.c:1065).
 * Userspace side it is an ABI change: see
 * ../packaging/rdma-core-patches/0004-providers-rxe-bind-to-usb4_rdma-devices.patch
 * -- thunderbolt_frame_rxe.ko and rdma-core must be rolled together.
 */
#define RDMA_DRIVER_USB4_RDMA	((enum rdma_driver_id)0x55534234)	/* "USB4" */

#define rxe_dbg(fmt, ...) pr_debug("%s: " fmt, __func__, ##__VA_ARGS__)
#define rxe_dbg_dev(rxe, fmt, ...) ibdev_dbg(&(rxe)->ib_dev,		\
		"%s: " fmt, __func__, ##__VA_ARGS__)
#define rxe_dbg_uc(uc, fmt, ...) ibdev_dbg((uc)->ibuc.device,		\
		"uc#%d %s: " fmt, (uc)->elem.index, __func__, ##__VA_ARGS__)
#define rxe_dbg_pd(pd, fmt, ...) ibdev_dbg((pd)->ibpd.device,		\
		"pd#%d %s: " fmt, (pd)->elem.index, __func__, ##__VA_ARGS__)
#define rxe_dbg_ah(ah, fmt, ...) ibdev_dbg((ah)->ibah.device,		\
		"ah#%d %s: " fmt, (ah)->elem.index, __func__, ##__VA_ARGS__)
#define rxe_dbg_srq(srq, fmt, ...) ibdev_dbg((srq)->ibsrq.device,	\
		"srq#%d %s: " fmt, (srq)->elem.index, __func__, ##__VA_ARGS__)
#define rxe_dbg_qp(qp, fmt, ...) ibdev_dbg((qp)->ibqp.device,		\
		"qp#%d %s: " fmt, (qp)->elem.index, __func__, ##__VA_ARGS__)
#define rxe_dbg_cq(cq, fmt, ...) ibdev_dbg((cq)->ibcq.device,		\
		"cq#%d %s: " fmt, (cq)->elem.index, __func__, ##__VA_ARGS__)
#define rxe_dbg_mr(mr, fmt, ...) ibdev_dbg((mr)->ibmr.device,		\
		"mr#%d %s:  " fmt, (mr)->elem.index, __func__, ##__VA_ARGS__)
#define rxe_dbg_mw(mw, fmt, ...) ibdev_dbg((mw)->ibmw.device,		\
		"mw#%d %s:  " fmt, (mw)->elem.index, __func__, ##__VA_ARGS__)

#define rxe_err(fmt, ...) pr_err_ratelimited("%s: " fmt, __func__, \
					##__VA_ARGS__)
#define rxe_err_dev(rxe, fmt, ...) ibdev_err_ratelimited(&(rxe)->ib_dev, \
		"%s: " fmt, __func__, ##__VA_ARGS__)
#define rxe_err_uc(uc, fmt, ...) ibdev_err_ratelimited((uc)->ibuc.device, \
		"uc#%d %s: " fmt, (uc)->elem.index, __func__, ##__VA_ARGS__)
#define rxe_err_pd(pd, fmt, ...) ibdev_err_ratelimited((pd)->ibpd.device, \
		"pd#%d %s: " fmt, (pd)->elem.index, __func__, ##__VA_ARGS__)
#define rxe_err_ah(ah, fmt, ...) ibdev_err_ratelimited((ah)->ibah.device, \
		"ah#%d %s: " fmt, (ah)->elem.index, __func__, ##__VA_ARGS__)
#define rxe_err_srq(srq, fmt, ...) ibdev_err_ratelimited((srq)->ibsrq.device, \
		"srq#%d %s: " fmt, (srq)->elem.index, __func__, ##__VA_ARGS__)
#define rxe_err_qp(qp, fmt, ...) ibdev_err_ratelimited((qp)->ibqp.device, \
		"qp#%d %s: " fmt, (qp)->elem.index, __func__, ##__VA_ARGS__)
#define rxe_err_cq(cq, fmt, ...) ibdev_err_ratelimited((cq)->ibcq.device, \
		"cq#%d %s: " fmt, (cq)->elem.index, __func__, ##__VA_ARGS__)
#define rxe_err_mr(mr, fmt, ...) ibdev_err_ratelimited((mr)->ibmr.device, \
		"mr#%d %s:  " fmt, (mr)->elem.index, __func__, ##__VA_ARGS__)
#define rxe_err_mw(mw, fmt, ...) ibdev_err_ratelimited((mw)->ibmw.device, \
		"mw#%d %s:  " fmt, (mw)->elem.index, __func__, ##__VA_ARGS__)

#define rxe_info(fmt, ...) pr_info_ratelimited("%s: " fmt, __func__, \
					##__VA_ARGS__)
#define rxe_info_dev(rxe, fmt, ...) ibdev_info_ratelimited(&(rxe)->ib_dev, \
		"%s: " fmt, __func__, ##__VA_ARGS__)
#define rxe_info_uc(uc, fmt, ...) ibdev_info_ratelimited((uc)->ibuc.device, \
		"uc#%d %s: " fmt, (uc)->elem.index, __func__, ##__VA_ARGS__)
#define rxe_info_pd(pd, fmt, ...) ibdev_info_ratelimited((pd)->ibpd.device, \
		"pd#%d %s: " fmt, (pd)->elem.index, __func__, ##__VA_ARGS__)
#define rxe_info_ah(ah, fmt, ...) ibdev_info_ratelimited((ah)->ibah.device, \
		"ah#%d %s: " fmt, (ah)->elem.index, __func__, ##__VA_ARGS__)
#define rxe_info_srq(srq, fmt, ...) ibdev_info_ratelimited((srq)->ibsrq.device, \
		"srq#%d %s: " fmt, (srq)->elem.index, __func__, ##__VA_ARGS__)
#define rxe_info_qp(qp, fmt, ...) ibdev_info_ratelimited((qp)->ibqp.device, \
		"qp#%d %s: " fmt, (qp)->elem.index, __func__, ##__VA_ARGS__)
#define rxe_info_cq(cq, fmt, ...) ibdev_info_ratelimited((cq)->ibcq.device, \
		"cq#%d %s: " fmt, (cq)->elem.index, __func__, ##__VA_ARGS__)
#define rxe_info_mr(mr, fmt, ...) ibdev_info_ratelimited((mr)->ibmr.device, \
		"mr#%d %s:  " fmt, (mr)->elem.index, __func__, ##__VA_ARGS__)
#define rxe_info_mw(mw, fmt, ...) ibdev_info_ratelimited((mw)->ibmw.device, \
		"mw#%d %s:  " fmt, (mw)->elem.index, __func__, ##__VA_ARGS__)

void rxe_set_mtu(struct rxe_dev *rxe, unsigned int frame_payload);

/* Initialize a freshly ib_alloc_device()d rxe_dev. @mac seeds the node and
 * port GUIDs (the GID-anchor netdev's identity MAC). Registration happens
 * separately via rxe_register_device() (tbrxe_frame.c, at link_up).
 */
int rxe_add(struct rxe_dev *rxe, unsigned int mtu, const u8 *mac);

void rxe_rcv(struct sk_buff *skb);

void rxe_port_up(struct rxe_dev *rxe);
void rxe_port_down(struct rxe_dev *rxe);

/* Report the Thunderbolt link's real rate through the IB port attributes.
 * Consumers size their cost model from active_speed x active_width -- NCCL
 * literally computes ncclIbSpeed(active_speed) * ncclIbWidth(active_width)
 * -- so leaving these at the scaffold's SDR/1X advertises 2.5 Gb/s for a
 * 20 Gb/s rail and ranks it below stock rxe_lan, which reports a nominal
 * 10 Gb/s over a 2.5GbE NIC. Measured 2026-08-25: every PP hop, including
 * ones with a healthy 1472-1695 MB/s rail, was placed on rxe_lan.
 */
void tbrxe_link_ib_rate(u8 tb_speed_gbps, u8 tb_lanes,
			u8 *active_speed, u8 *active_width);
void tbrxe_rxe_set_link_rate(struct rxe_dev *rxe, u8 tb_speed_gbps,
			     u8 tb_lanes);

#endif /* RXE_H */
