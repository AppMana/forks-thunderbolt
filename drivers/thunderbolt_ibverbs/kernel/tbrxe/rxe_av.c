// SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB
/*
 * Copyright (c) 2016 Mellanox Technologies Ltd. All rights reserved.
 * Copyright (c) 2015 System Fabric Works, Inc. All rights reserved.
 */

#include "rxe.h"
#include "rxe_loc.h"

void rxe_init_av(struct rdma_ah_attr *attr, struct rxe_av *av)
{
	rxe_av_from_attr(rdma_ah_get_port_num(attr), av, attr);
	rxe_av_fill_ip_info(av, attr);
	/* tbrxe: no dmac. The port is IB link layer (GID addressed); the
	 * transport resolves the dgid through the tbframe peer table.
	 */
}

static int chk_attr(void *obj, struct rdma_ah_attr *attr, bool obj_is_ah)
{
	const struct ib_global_route *grh = rdma_ah_read_grh(attr);
	struct rxe_port *port;
	struct rxe_dev *rxe;
	struct rxe_qp *qp;
	struct rxe_ah *ah;
	int type;

	if (obj_is_ah) {
		ah = obj;
		rxe = to_rdev(ah->ibah.device);
	} else {
		qp = obj;
		rxe = to_rdev(qp->ibqp.device);
	}

	port = &rxe->port;

	/* tbrxe is GID-addressed only (wire-spec section 8): the transport
	 * routes by dgid through the tbframe peer table, so every AV must
	 * carry a GRH with a core-resolved sgid_attr. LID addressing does
	 * not exist on this fabric; a GRH-less AV (perftest without -x on an
	 * IB link-layer port) used to reach rxe_av_fill_ip_info and deref
	 * the NULL sgid_attr (appmana-023 kdump 202608031446). The port
	 * advertises RDMA_CORE_CAP_IB_GRH_REQUIRED; this is the backstop.
	 */
	if (!(rdma_ah_get_ah_flags(attr) & IB_AH_GRH) || !grh->sgid_attr) {
		if (obj_is_ah)
			rxe_dbg_ah(ah, "AV without GRH/sgid_attr on a GID-addressed fabric\n");
		else
			rxe_dbg_qp(qp, "AV without GRH/sgid_attr on a GID-addressed fabric\n");
		return -EINVAL;
	}

	if (grh->sgid_index > port->attr.gid_tbl_len) {
		if (obj_is_ah)
			rxe_dbg_ah(ah, "invalid sgid index = %d\n",
					grh->sgid_index);
		else
			rxe_dbg_qp(qp, "invalid sgid index = %d\n",
					grh->sgid_index);
		return -EINVAL;
	}

	/* tbrxe: the port is IB link layer, so ib_core hands us
	 * RDMA_NETWORK_IB GIDs (the per-link ULAs); they behave as
	 * IPv6 addresses everywhere below. RoCEv1 stays invalid.
	 */
	type = rdma_gid_attr_network_type(grh->sgid_attr);
	if (type != RDMA_NETWORK_IB &&
	    (type < RDMA_NETWORK_IPV4 || type > RDMA_NETWORK_IPV6)) {
		if (obj_is_ah)
			rxe_dbg_ah(ah, "invalid network type for tbrxe = %d\n",
					type);
		else
			rxe_dbg_qp(qp, "invalid network type for tbrxe = %d\n",
					type);
		return -EINVAL;
	}

	return 0;
}

int rxe_av_chk_attr(struct rxe_qp *qp, struct rdma_ah_attr *attr)
{
	return chk_attr(qp, attr, false);
}

int rxe_ah_chk_attr(struct rxe_ah *ah, struct rdma_ah_attr *attr)
{
	return chk_attr(ah, attr, true);
}

void rxe_av_from_attr(u8 port_num, struct rxe_av *av,
		     struct rdma_ah_attr *attr)
{
	const struct ib_global_route *grh = rdma_ah_read_grh(attr);

	memset(av, 0, sizeof(*av));
	memcpy(av->grh.dgid.raw, grh->dgid.raw, sizeof(grh->dgid.raw));
	av->grh.flow_label = grh->flow_label;
	av->grh.sgid_index = grh->sgid_index;
	av->grh.hop_limit = grh->hop_limit;
	av->grh.traffic_class = grh->traffic_class;
	av->port_num = port_num;
}

void rxe_av_to_attr(struct rxe_av *av, struct rdma_ah_attr *attr)
{
	struct ib_global_route *grh = rdma_ah_retrieve_grh(attr);

	attr->type = RDMA_AH_ATTR_TYPE_IB;

	memcpy(grh->dgid.raw, av->grh.dgid.raw, sizeof(av->grh.dgid.raw));
	grh->flow_label = av->grh.flow_label;
	grh->sgid_index = av->grh.sgid_index;
	grh->hop_limit = av->grh.hop_limit;
	grh->traffic_class = av->grh.traffic_class;

	rdma_ah_set_ah_flags(attr, IB_AH_GRH);
	rdma_ah_set_port_num(attr, av->port_num);
}

void rxe_av_fill_ip_info(struct rxe_av *av, struct rdma_ah_attr *attr)
{
	const struct ib_gid_attr *sgid_attr = attr->grh.sgid_attr;
	int ibtype;
	int type;

	rdma_gid2ip((struct sockaddr *)&av->sgid_addr, &sgid_attr->gid);
	rdma_gid2ip((struct sockaddr *)&av->dgid_addr,
		    &rdma_ah_read_grh(attr)->dgid);

	ibtype = rdma_gid_attr_network_type(sgid_attr);

	switch (ibtype) {
	case RDMA_NETWORK_IPV4:
		type = RXE_NETWORK_TYPE_IPV4;
		break;
	case RDMA_NETWORK_IB:
		/* tbrxe: IB-typed ULA GIDs are plain IPv6 addresses. */
	case RDMA_NETWORK_IPV6:
		type = RXE_NETWORK_TYPE_IPV6;
		break;
	default:
		/* not reached - checked in rxe_av_chk_attr */
		type = 0;
		break;
	}

	av->network_type = type;
}

struct rxe_av *rxe_get_av(struct rxe_pkt_info *pkt, struct rxe_ah **ahp)
{
	struct rxe_ah *ah;
	u32 ah_num;

	if (ahp)
		*ahp = NULL;

	if (!pkt || !pkt->qp)
		return NULL;

	if (qp_type(pkt->qp) == IB_QPT_RC || qp_type(pkt->qp) == IB_QPT_UC)
		return &pkt->qp->pri_av;

	if (!pkt->wqe)
		return NULL;

	ah_num = pkt->wqe->wr.wr.ud.ah_num;
	if (ah_num) {
		/* only new user provider or kernel client */
		ah = rxe_pool_get_index(&pkt->rxe->ah_pool, ah_num);
		if (!ah) {
			rxe_dbg_qp(pkt->qp, "Unable to find AH matching ah_num\n");
			return NULL;
		}

		if (rxe_ah_pd(ah) != pkt->qp->pd) {
			rxe_dbg_qp(pkt->qp, "PDs don't match for AH and QP\n");
			rxe_put(ah);
			return NULL;
		}

		if (ahp)
			*ahp = ah;
		else
			rxe_put(ah);

		return &ah->av;
	}

	/* only old user provider for UD sends*/
	return &pkt->wqe->wr.wr.ud.av;
}
