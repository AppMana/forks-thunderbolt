// SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB
/*
 * tbrxe_frame.c - tbframe transport for the rxe-derived tbrxe IB engine.
 *
 * Replaces rxe_net.c. The engine <-> transport boundary is unchanged from
 * upstream rxe (4 downcalls: rxe_init_packet, rxe_prepare, rxe_xmit_packet,
 * rxe_parent_name; upcalls: rxe_rcv, port events, tx wakeups), but the wire
 * below BTH is the tbframe frame service instead of UDP/IP over a netdev.
 * Normative wire format: docs/tbframe-tbrxe-wire-spec.md. One frame carries
 * exactly BTH..ICRC; there is no encapsulation header.
 *
 * Addressing model (wire-spec section 8, normative): ONE ib_device PER
 * tbframe link, published at that link's first link_up and unpublished on
 * terminal link_down. Each device is bound (ib_device_set_netdev) to a
 * GID-anchor netdev: IFF_NOARP, xmit drops, carries no data ever. ib_core
 * populates the RoCE GID table from that netdev's IPv6 addresses -- the
 * link-local from the per-link identity MAC plus the deterministic
 * per-cable ULA the userspace tooling (tbv-rdma-addr) assigns -- and RC
 * modify_qp(RTR) resolves over the netdev's routes. The driver carries no
 * GID identity of its own and needs no peer table: the device IS the link,
 * so the transport for any non-loopback packet is the device's link.
 *
 * The skb seam
 * ------------
 * TX (wire): the packet payload (BTH..ICRC) lives in a tbframe TX frame;
 *   pkt->hdr points at frame->data so the engine composes directly into the
 *   ring buffer (zero copy). The sk_buff is only a carrier ("shell"): its
 *   data area holds a struct tbrxe_txinfo {link, frame}, its len stays 0,
 *   and its destructor returns the frame to tbframe if the engine drops the
 *   skb before transmission. Engines never look at skb->data/len on TX;
 *   they use pkt->hdr exclusively.
 *
 * TX (loopback): a normal skb; pkt->hdr = skb_put(paylen). Chosen when the
 *   AV's dgid equals its sgid (self-addressed) or is multicast.
 *
 * RX: one copy from the tbframe RX frame into a fresh skb inside the rx()
 *   upcall, so the frame can be reposted immediately and the engine owns a
 *   normal skb through its req_pkts/resp_pkts queues.
 *
 * Both RX and loopback skbs carry a synthetic IPv6 header in headroom
 * (network header set, skb->protocol = ETH_P_IPV6, skb->dev =
 * blackhole_netdev) so the verbatim engine paths that peek at it keep
 * working. The wire carries no IP addresses (BTH..ICRC only, by design),
 * so for wire frames the addresses are DIAGNOSTIC link-locals derived from
 * the session's HELLO identities; ring ownership, the QP number, the PSN
 * machinery and the ICRC are what validate a wire frame, and rxe_recv.c
 * skips exact address matching for non-loopback packets accordingly.
 * Loopback skbs carry the exact AV addresses.
 *
 * Backpressure: tbframe admission (alloc_frame -> -ENOSPC) bounds the
 * local TX ring, and the engine-side Mode A accounting below bounds the
 * aggregate unacked packets against the peer's advertised window. Both
 * park the QP via the existing need_req_skb flag; the tx_released upcall
 * and ACK-driven release reschedule it.
 */

#include <linux/etherdevice.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <rdma/ib_addr.h>

#include "rxe.h"
#include "rxe_loc.h"
#include "tbrxe_frame.h"

/* Headroom for the synthetic IPv6 header on RX/loopback skbs. */
#define TBRXE_GRH_BYTES		sizeof(struct ipv6hdr)

/*
 * Strict IB_MTU_2048 (wire-spec section 5): worst-case transport unit that
 * a link must be able to carry in one frame.
 */
#define TBRXE_MIN_LINK_PAYLOAD	(RXE_MAX_HDR_LENGTH + 2048 + 3 + RXE_ICRC_SIZE)

/*
 * One record per tbframe link: the link's ib_device, its GID-anchor netdev
 * and the Mode A admission state. Created (and the device published) at
 * the link's first link_up; destroyed (device unpublished) on terminal
 * link_down. Non-terminal session bounces only toggle the port state.
 */
struct tbrxe_link {
	struct list_head	list;
	struct tbframe_link	*tblink;
	struct tbframe_link_info info;
	struct rxe_dev		*rxe;
	struct net_device	*ndev;
	bool			session_up;
	/* Mode A engine-side admission, under tbrxe.lock. */
	u32			unacked;
	bool			admission_waiters;
};

/* Lives in the TX shell skb's data area (never in skb->cb: that holds the
 * engine's rxe_pkt_info and is full).
 */
struct tbrxe_txinfo {
	struct tbrxe_link	*link;
	struct tbframe_frame	*frame;
};

static struct {
	spinlock_t		lock;	/* links list, admission counters */
	struct list_head	links;
	const struct tbrxe_transport_ops *ops;
	bool			registered;
	/* serializes record create/teardown across link_up/link_down */
	struct mutex		publish_lock;
} tbrxe = {
	.lock	= __SPIN_LOCK_UNLOCKED(tbrxe.lock),
	.links	= LIST_HEAD_INIT(tbrxe.links),
	.publish_lock = __MUTEX_INITIALIZER(tbrxe.publish_lock),
};

static void tbrxe_kick_parked_qps(struct rxe_dev *rxe);

/* Weak: the production binding in tbrxe_tbframe_glue.c overrides this. */
const struct tbrxe_transport_ops * __weak tbrxe_builtin_transport(void)
{
	return NULL;
}

void tbrxe_set_transport_ops(const struct tbrxe_transport_ops *ops)
{
	unsigned long flags;

	spin_lock_irqsave(&tbrxe.lock, flags);
	tbrxe.ops = ops;
	spin_unlock_irqrestore(&tbrxe.lock, flags);
}

/* Caller holds tbrxe.lock. */
static struct tbrxe_link *tbrxe_find_record_locked(struct tbframe_link *tblink)
{
	struct tbrxe_link *link;

	list_for_each_entry(link, &tbrxe.links, list)
		if (link->tblink == tblink)
			return link;

	return NULL;
}

/* Caller holds tbrxe.lock. */
static bool tbrxe_link_is_registered_locked(struct tbrxe_link *link)
{
	struct tbrxe_link *l;

	list_for_each_entry(l, &tbrxe.links, list)
		if (l == link)
			return true;

	return false;
}

struct rxe_dev *tbrxe_link_device(struct tbframe_link *tblink)
{
	struct tbrxe_link *link;
	struct rxe_dev *rxe = NULL;
	unsigned long flags;

	spin_lock_irqsave(&tbrxe.lock, flags);
	link = tbrxe_find_record_locked(tblink);
	if (link)
		rxe = link->rxe;
	spin_unlock_irqrestore(&tbrxe.lock, flags);
	return rxe;
}

/* ---- Mode A engine-side admission (wire-spec section 6) ---------------- */

/*
 * The tbframe data window only bounds frames the local TX ring has not
 * completed; local TX completion proves the local NHI consumed the frame,
 * not the peer. The engine is the layer that sees ACKs, so it enforces the
 * normative invariant here: the aggregate unacked wire packets charged
 * against one link never exceed the peer's advertised window, and window
 * release is correlated with actual peer consumption (the peer reposts the
 * RX descriptor before its engine emits the ACK, so unacked <= window
 * implies its RX ring cannot overflow). No credit messages exist.
 *
 * Accounting: each RC wire packet pre-charges one slot in tbrxe_admit();
 * tbrxe_unacked_sync() then reconciles the QP's charge with its live PSN
 * distance (req.psn - comp.psn), which releases slots as the completer
 * advances comp.psn (ACK arrival), on retry rewind (req.psn pulled back to
 * comp.psn -- the rewound packets will be re-sent and re-charged, matching
 * the descriptors they will re-occupy), and tops up multi-response charges
 * (a READ advances req.psn by the number of response packets). QP
 * reset/error/destroy sync with a zero distance, returning everything.
 *
 * The record outlives every QP of its device (teardown unregisters the
 * ib_device, which drains all QPs, before freeing the record), so
 * rxe->tbl_link is always safe to dereference from engine context.
 *
 * Transient slack: between the pre-charge and update_wqe_psn the charge
 * exceeds the PSN distance by one, so a concurrent completer sync can
 * momentarily release that slot early. The overshoot is bounded by the
 * number of concurrently-sending QPs and absorbed by the control reserve
 * the ring is sized with.
 */

static u32 tbrxe_link_engine_window(const struct tbrxe_link *link)
{
	return link->info.data_window ? : 1;
}

bool tbrxe_admit(struct rxe_qp *qp)
{
	struct tbrxe_link *link = to_rdev(qp->ibqp.device)->tbl_link;
	unsigned long flags;
	bool ok = true;

	if (!link)
		return true;

	spin_lock_irqsave(&tbrxe.lock, flags);
	if (link->unacked >= tbrxe_link_engine_window(link)) {
		link->admission_waiters = true;
		ok = false;
	} else {
		link->unacked++;
		qp->tbl_charged++;
	}
	spin_unlock_irqrestore(&tbrxe.lock, flags);
	return ok;
}

void tbrxe_unacked_sync(struct rxe_qp *qp)
{
	struct tbrxe_link *link;
	struct rxe_dev *rxe;
	unsigned long flags;
	bool kick = false;
	u32 target;

	if (qp_type(qp) != IB_QPT_RC)
		return;

	rxe = to_rdev(qp->ibqp.device);
	link = rxe->tbl_link;
	if (!link)
		return;

	/* An errored/reset/destroyed QP will never see further ACKs; its
	 * in-flight frames are consumed (and their descriptors reposted) by
	 * the peer regardless of engine progress, so holding window for it
	 * only starves the link's live QPs. Racy state read is fine: the
	 * flush paths re-sync, destroy is the backstop.
	 */
	if (!qp->valid || qp->attr.qp_state == IB_QPS_ERR ||
	    qp->attr.qp_state == IB_QPS_RESET)
		target = 0;
	else
		target = (qp->req.psn - qp->comp.psn) & BTH_PSN_MASK;

	spin_lock_irqsave(&tbrxe.lock, flags);
	if (qp->tbl_charged > target) {
		u32 rel = qp->tbl_charged - target;

		link->unacked -= min(rel, link->unacked);
		qp->tbl_charged = target;
		if (link->admission_waiters &&
		    link->unacked < tbrxe_link_engine_window(link)) {
			link->admission_waiters = false;
			kick = true;
		}
	} else if (qp->tbl_charged < target) {
		/* Multi-response ops (READ) advance req.psn by more than the
		 * one packet admitted; top the charge up so release stays
		 * exact. May transiently overshoot the window; the gate in
		 * tbrxe_admit() re-closes admission.
		 */
		link->unacked += target - qp->tbl_charged;
		qp->tbl_charged = target;
	}
	spin_unlock_irqrestore(&tbrxe.lock, flags);

	if (kick)
		tbrxe_kick_parked_qps(rxe);
}

/* ---- GID-anchor netdev ------------------------------------------------- */

/*
 * Per-device virtual netdev whose only job is anchoring the ib_device's
 * RoCE GID table (ib_core populates GIDs from its IPv6 addresses) and the
 * routes RTR resolves over. It never carries data: the data path is the
 * tbframe link. The legacy driver's proven pattern (ibdev.c u4r%d rails):
 * IFF_NOARP, no queue, xmit drops, left admin-DOWN for udev to rename
 * (tbr-<peer>) and assign the deterministic per-cable ULA (tbv-rdma-addr).
 */

struct tbrxe_ndev_priv {
	char	peer_uuid[UUID_STRING_LEN + 1];
	char	peer_name[sizeof_field(struct tbframe_link_info, remote_name)];
};

static netdev_tx_t tbrxe_ndev_xmit(struct sk_buff *skb,
				   struct net_device *ndev)
{
	/* GID-anchor only: the data path is the tbframe link. */
	kfree_skb(skb);
	return NETDEV_TX_OK;
}

static const struct net_device_ops tbrxe_ndev_ops = {
	.ndo_start_xmit	= tbrxe_ndev_xmit,
};

static void tbrxe_ndev_setup(struct net_device *ndev)
{
	ether_setup(ndev);
	ndev->netdev_ops = &tbrxe_ndev_ops;
	ndev->flags |= IFF_NOARP;
	ndev->priv_flags |= IFF_NO_QUEUE;
}

/* Peer identity attrs consumed by the udev naming/addressing helpers
 * (tbv-rdma-ifname, tbv-rdma-addr).
 */
static ssize_t tbv_peer_uuid_show(struct device *d,
				  struct device_attribute *a, char *buf)
{
	struct tbrxe_ndev_priv *priv = netdev_priv(to_net_dev(d));

	return sysfs_emit(buf, "%s\n", priv->peer_uuid);
}
static DEVICE_ATTR_RO(tbv_peer_uuid);

static ssize_t tbv_peer_name_show(struct device *d,
				  struct device_attribute *a, char *buf)
{
	struct tbrxe_ndev_priv *priv = netdev_priv(to_net_dev(d));

	return sysfs_emit(buf, "%s\n", priv->peer_name);
}
static DEVICE_ATTR_RO(tbv_peer_name);

static struct attribute *tbrxe_ndev_attrs[] = {
	&dev_attr_tbv_peer_uuid.attr,
	&dev_attr_tbv_peer_name.attr,
	NULL,
};
static const struct attribute_group tbrxe_ndev_group = {
	.attrs = tbrxe_ndev_attrs,
};

/* The identity MAC behind the advertised EUI-64 (RFC 4291 modified EUI-64
 * inverted): the netdev's link-local GID then equals the HELLO identity.
 */
static void tbrxe_mac_from_eui64(u64 eui64, u8 mac[ETH_ALEN])
{
	mac[0] = ((eui64 >> 56) & 0xff) ^ 0x02;
	mac[1] = eui64 >> 48;
	mac[2] = eui64 >> 40;
	mac[3] = eui64 >> 16;
	mac[4] = eui64 >> 8;
	mac[5] = eui64;
}

static struct net_device *tbrxe_ndev_create(const struct tbframe_link_info *info)
{
	struct tbrxe_ndev_priv *priv;
	struct net_device *ndev;
	u8 mac[ETH_ALEN];
	int err;

	ndev = alloc_netdev(sizeof(*priv), "u4r%d", NET_NAME_ENUM,
			    tbrxe_ndev_setup);
	if (!ndev)
		return ERR_PTR(-ENOMEM);

	priv = netdev_priv(ndev);
	snprintf(priv->peer_uuid, sizeof(priv->peer_uuid), "%pUb",
		 info->remote_uuid);
	strscpy(priv->peer_name, info->remote_name, sizeof(priv->peer_name));

	tbrxe_mac_from_eui64(info->local_gid_eui64, mac);
	eth_hw_addr_set(ndev, mac);

	ndev->sysfs_groups[0] = &tbrxe_ndev_group;

	err = register_netdev(ndev);
	if (err) {
		free_netdev(ndev);
		return ERR_PTR(err);
	}
	return ndev;
}

/* ---- packet paths ------------------------------------------------------ */

/*
 * Local delivery test: self-addressed (dgid == sgid) or multicast. The
 * only mcast consumers are self-attached UD groups (rxe_mcast.c); fanning
 * multicast out over the link is an open item.
 */
static bool tbrxe_dest_is_local(const struct rxe_av *av)
{
	return rdma_is_multicast_addr(
		       (struct in6_addr *)&av->grh.dgid) ||
	       ipv6_addr_equal(&av->sgid_addr._sockaddr_in6.sin6_addr,
			       &av->dgid_addr._sockaddr_in6.sin6_addr);
}

/* Diagnostic link-local from a HELLO EUI-64 identity, for the synthetic
 * IPv6 header on wire-RX skbs. The wire carries no addresses (BTH..ICRC
 * only); these are for the UD GRH copy and wc fields, never validated.
 */
static void tbrxe_diag_addr(u64 eui64, struct in6_addr *addr)
{
	memset(addr, 0, sizeof(*addr));
	addr->s6_addr[0] = 0xfe;
	addr->s6_addr[1] = 0x80;
	put_unaligned_be64(eui64, &addr->s6_addr[8]);
}

/*
 * Synthetic IPv6 header standing in for the GRH, written into headroom so
 * skb->len stays paylen (== BTH..ICRC), matching what rxe_rcv() expects
 * after the upstream UDP pull.
 */
static void tbrxe_fill_grh(struct sk_buff *skb, const struct in6_addr *saddr,
			   const struct in6_addr *daddr, u16 paylen)
{
	struct ipv6hdr *ip6h;

	skb_set_network_header(skb, -(int)TBRXE_GRH_BYTES);
	ip6h = ipv6_hdr(skb);
	memset(ip6h, 0, sizeof(*ip6h));
	ip6h->version = 6;
	ip6h->payload_len = htons(paylen);
	ip6h->nexthdr = IPPROTO_UDP;
	ip6h->hop_limit = 0xff;
	ip6h->saddr = *saddr;
	ip6h->daddr = *daddr;

	skb->protocol = htons(ETH_P_IPV6);
	/* Non-NULL, non-VLAN, never unregistered: keeps the verbatim
	 * is_vlan_dev(skb->dev) in rxe_resp.c:do_complete() safe.
	 */
	skb->dev = blackhole_netdev;
}

/* TX shell destructor: return a frame the engine dropped unsent. */
static void tbrxe_skb_tx_dtor(struct sk_buff *skb)
{
	struct tbrxe_txinfo *txi = (struct tbrxe_txinfo *)skb->data;
	unsigned long flags;

	if (!txi->frame)
		return;

	spin_lock_irqsave(&tbrxe.lock, flags);
	if (tbrxe.ops && tbrxe_link_is_registered_locked(txi->link))
		tbrxe.ops->frame_free(txi->link->tblink, txi->frame);
	spin_unlock_irqrestore(&tbrxe.lock, flags);
	txi->frame = NULL;
}

/*
 * Downcall 1: allocate a frame with paylen writable bytes for BTH..ICRC.
 * The transport for any non-loopback packet is the device's own link (the
 * device IS the link, wire-spec section 8); there is no GID lookup.
 * ACK-class packets (RXE_ACK_MASK: acknowledges and read responses) are
 * charged to the tbframe control reserve via is_ctrl.
 */
struct sk_buff *rxe_init_packet(struct rxe_dev *rxe, struct rxe_av *av,
				int paylen, struct rxe_pkt_info *pkt)
{
	bool is_ctrl = !!(pkt->mask & RXE_ACK_MASK);
	struct tbframe_frame *frame = NULL;
	struct tbrxe_link *link;
	struct tbrxe_txinfo *txi;
	struct sk_buff *skb;
	unsigned long flags;
	int err;

	if (tbrxe_dest_is_local(av)) {
		skb = alloc_skb(TBRXE_GRH_BYTES + paylen, GFP_ATOMIC);
		if (unlikely(!skb))
			return NULL;

		skb_reserve(skb, TBRXE_GRH_BYTES);
		tbrxe_fill_grh(skb, &av->sgid_addr._sockaddr_in6.sin6_addr,
			       &av->dgid_addr._sockaddr_in6.sin6_addr,
			       paylen);
		pkt->hdr = skb_put(skb, paylen);
	} else {
		link = rxe->tbl_link;

		spin_lock_irqsave(&tbrxe.lock, flags);
		if (!link || !tbrxe.ops || !link->session_up) {
			spin_unlock_irqrestore(&tbrxe.lock, flags);
			return NULL;	/* down: engine retries / errors */
		}

		err = tbrxe.ops->alloc_frame(link->tblink, paylen, is_ctrl,
					     &frame);
		spin_unlock_irqrestore(&tbrxe.lock, flags);
		if (err) {
			if (err == -ENOSPC && pkt->qp) {
				/* Admission window closed: park the QP;
				 * tx_released() reschedules it.
				 */
				pkt->qp->need_req_skb = 1;
				smp_wmb();
			}
			return NULL;
		}

		skb = alloc_skb(sizeof(struct tbrxe_txinfo), GFP_ATOMIC);
		if (unlikely(!skb)) {
			spin_lock_irqsave(&tbrxe.lock, flags);
			if (tbrxe.ops && tbrxe_link_is_registered_locked(link))
				tbrxe.ops->frame_free(link->tblink, frame);
			spin_unlock_irqrestore(&tbrxe.lock, flags);
			return NULL;
		}

		txi = (struct tbrxe_txinfo *)skb->data;
		txi->link = link;
		txi->frame = frame;
		skb->destructor = tbrxe_skb_tx_dtor;

		pkt->hdr = frame->data;
	}

	pkt->rxe = rxe;
	pkt->port_num = 1;
	pkt->mask |= RXE_GRH_MASK;

	return skb;
}

/* Downcall 2: mark loopback; wire packets ride the device's link. */
int rxe_prepare(struct rxe_av *av, struct rxe_pkt_info *pkt,
		struct sk_buff *skb)
{
	struct tbrxe_txinfo *txi;
	unsigned long flags;
	int err = 0;

	if (tbrxe_dest_is_local(av)) {
		pkt->mask |= RXE_LOOPBACK_MASK;
		return 0;
	}

	txi = (struct tbrxe_txinfo *)skb->data;

	spin_lock_irqsave(&tbrxe.lock, flags);
	if (!tbrxe_link_is_registered_locked(txi->link))
		err = -EHOSTUNREACH;
	spin_unlock_irqrestore(&tbrxe.lock, flags);

	return err;
}

/* Loopback: feed the finished packet straight back into the rx ladder. */
static int tbrxe_loopback(struct sk_buff *skb, struct rxe_pkt_info *pkt)
{
	memcpy(SKB_TO_PKT(skb), pkt, sizeof(*pkt));

	if (WARN_ON(!ib_device_try_get(&pkt->rxe->ib_dev))) {
		kfree_skb(skb);
		return -EIO;
	}

	rxe_rcv(skb);

	return 0;
}

static int tbrxe_send(struct sk_buff *skb, struct rxe_pkt_info *pkt)
{
	struct tbrxe_txinfo *txi = (struct tbrxe_txinfo *)skb->data;
	struct tbframe_frame *frame = txi->frame;
	unsigned long flags;
	int err;

	spin_lock_irqsave(&tbrxe.lock, flags);
	if (!tbrxe.ops || !tbrxe_link_is_registered_locked(txi->link)) {
		/* Link went down; its frames are gone per the tbframe
		 * contract (link_down cancels everything in flight).
		 */
		txi->frame = NULL;
		spin_unlock_irqrestore(&tbrxe.lock, flags);
		kfree_skb(skb);
		return -ENETDOWN;
	}

	frame->len = pkt->paylen;
	frame->pdf = TBFRAME_PDF_DATA;
	frame->is_ctrl = !!(pkt->mask & RXE_ACK_MASK);

	err = tbrxe.ops->xmit(txi->link->tblink, frame);
	if (!err)
		txi->frame = NULL;	/* consumed by tbframe */
	spin_unlock_irqrestore(&tbrxe.lock, flags);

	kfree_skb(skb);		/* destructor frees the frame on error */
	return err;
}

/* Downcall 3: QP-state gate + ICRC + send or loopback, consuming the skb. */
int rxe_xmit_packet(struct rxe_qp *qp, struct rxe_pkt_info *pkt,
		    struct sk_buff *skb)
{
	int err;
	int is_request = pkt->mask & RXE_REQ_MASK;
	struct rxe_dev *rxe = to_rdev(qp->ibqp.device);
	unsigned long flags;

	spin_lock_irqsave(&qp->state_lock, flags);
	if ((is_request && (qp_state(qp) < IB_QPS_RTS)) ||
	    (!is_request && (qp_state(qp) < IB_QPS_RTR))) {
		spin_unlock_irqrestore(&qp->state_lock, flags);
		rxe_dbg_qp(qp, "Packet dropped. QP is not in ready state\n");
		goto drop;
	}
	spin_unlock_irqrestore(&qp->state_lock, flags);

	rxe_icrc_generate(skb, pkt);

	if (pkt->mask & RXE_LOOPBACK_MASK)
		err = tbrxe_loopback(skb, pkt);
	else
		err = tbrxe_send(skb, pkt);
	if (err) {
		rxe_counter_inc(rxe, RXE_CNT_SEND_ERR);
		return err;
	}

	rxe_counter_inc(rxe, RXE_CNT_SENT_PKTS);
	goto done;

drop:
	kfree_skb(skb);
	err = 0;
done:
	return err;
}

/* Downcall 4: sysfs "parent" attribute. */
const char *rxe_parent_name(struct rxe_dev *rxe, unsigned int port_num)
{
	struct tbrxe_link *link = rxe->tbl_link;

	if (link && tbrxe.ops && tbrxe.ops->link_name)
		return tbrxe.ops->link_name(link->tblink);
	return "tbframe";
}

/* ---- port events (same shape as rxe_net.c) ---------------------------- */

static void rxe_port_event(struct rxe_dev *rxe,
			   enum ib_event_type event)
{
	struct ib_event ev;

	ev.device = &rxe->ib_dev;
	ev.element.port_num = 1;
	ev.event = event;

	ib_dispatch_event(&ev);
}

void rxe_port_up(struct rxe_dev *rxe)
{
	rxe_port_event(rxe, IB_EVENT_PORT_ACTIVE);
	dev_info(&rxe->ib_dev.dev, "set active\n");
}

void rxe_port_down(struct rxe_dev *rxe)
{
	rxe_port_event(rxe, IB_EVENT_PORT_ERR);
	rxe_counter_inc(rxe, RXE_CNT_LINK_DOWNED);
	dev_info(&rxe->ib_dev.dev, "set down\n");
}

/* ---- tbframe client upcalls ------------------------------------------- */

/*
 * rx(): copy the frame into an skb shaped exactly like what upstream
 * rxe_udp_encap_recv() hands to rxe_rcv(), then run the verbatim ladder.
 */
static void tbrxe_client_rx(void *ctx, struct tbframe_link *tblink,
			    struct tbframe_frame *frame)
{
	struct tbrxe_link *link;
	struct in6_addr saddr, daddr;
	struct rxe_pkt_info *pkt;
	struct rxe_dev *rxe = NULL;
	struct sk_buff *skb;
	unsigned long flags;
	u16 len = frame->len;

	if (frame->pdf != TBFRAME_PDF_DATA)
		return;

	if (len < RXE_BTH_BYTES + RXE_ICRC_SIZE || len > TBFRAME_MAX_FRAME)
		return;

	spin_lock_irqsave(&tbrxe.lock, flags);
	link = tbrxe_find_record_locked(tblink);
	if (link && ib_device_try_get(&link->rxe->ib_dev)) {
		rxe = link->rxe;
		tbrxe_diag_addr(link->info.gid_eui64, &saddr);
		tbrxe_diag_addr(link->info.local_gid_eui64, &daddr);
	}
	spin_unlock_irqrestore(&tbrxe.lock, flags);
	if (!rxe)
		return;

	skb = alloc_skb(TBRXE_GRH_BYTES + len, GFP_ATOMIC);
	if (unlikely(!skb)) {
		ib_device_put(&rxe->ib_dev);
		return;
	}

	skb_reserve(skb, TBRXE_GRH_BYTES);
	tbrxe_fill_grh(skb, &saddr, &daddr, len);
	memcpy(skb_put(skb, len), frame->data, len);

	pkt = SKB_TO_PKT(skb);
	pkt->rxe = rxe;
	pkt->port_num = 1;
	pkt->hdr = skb->data;
	pkt->mask = RXE_GRH_MASK;
	pkt->paylen = len;

	rxe_rcv(skb);
}

/* Reschedule every parked QP of one device (window reopened). */
static void tbrxe_kick_parked_qps(struct rxe_dev *rxe)
{
	struct rxe_pool_elem *elem;
	unsigned long index;
	struct rxe_qp *qp;

	xa_for_each(&rxe->qp_pool.xa, index, elem) {
		qp = rxe_pool_get_index(&rxe->qp_pool, index);
		if (!qp)
			continue;
		if (qp->need_req_skb)
			rxe_sched_task(&qp->send_task);
		rxe_sched_task(&qp->recv_task);
		rxe_put(qp);
	}
}

/*
 * tx_released(): the link's tbframe admission window reopened. Kick the
 * link's own device only.
 */
static void tbrxe_client_tx_released(void *ctx, struct tbframe_link *tblink)
{
	struct tbrxe_link *link;
	struct rxe_dev *rxe = NULL;
	unsigned long flags;

	spin_lock_irqsave(&tbrxe.lock, flags);
	link = tbrxe_find_record_locked(tblink);
	if (link && ib_device_try_get(&link->rxe->ib_dev))
		rxe = link->rxe;
	spin_unlock_irqrestore(&tbrxe.lock, flags);
	if (!rxe)
		return;

	tbrxe_kick_parked_qps(rxe);
	ib_device_put(&rxe->ib_dev);
}

/*
 * link_up: first link_up of a tbframe link publishes that link's ib_device
 * (wire-spec section 8), bound to a fresh GID-anchor netdev; later
 * link_ups of the same link are session re-establishments and only toggle
 * the port state. Upcall context is tbframe's workqueue (process context,
 * no tbframe locks held), so sleeping registration is fine.
 */
static void tbrxe_client_link_up(void *ctx, struct tbframe_link *tblink,
				 const struct tbframe_link_info *info)
{
	struct tbrxe_link *link;
	struct net_device *ndev;
	struct rxe_dev *rxe;
	unsigned long flags;
	int err;

	if (info->max_payload < TBRXE_MIN_LINK_PAYLOAD) {
		pr_err("tbrxe: link %s max_payload %u below %u needed for IB_MTU_2048, ignoring link\n",
		       tbrxe.ops && tbrxe.ops->link_name ?
				tbrxe.ops->link_name(tblink) : "?",
		       info->max_payload,
		       (unsigned int)TBRXE_MIN_LINK_PAYLOAD);
		return;
	}

	mutex_lock(&tbrxe.publish_lock);

	spin_lock_irqsave(&tbrxe.lock, flags);
	link = tbrxe_find_record_locked(tblink);
	if (link) {
		link->info = *info;
		link->session_up = true;
		link->unacked = 0;
		link->admission_waiters = false;
	}
	spin_unlock_irqrestore(&tbrxe.lock, flags);
	if (link) {
		rxe_port_up(link->rxe);
		mutex_unlock(&tbrxe.publish_lock);
		return;
	}

	link = kzalloc(sizeof(*link), GFP_KERNEL);
	if (!link)
		goto out_unlock;
	link->tblink = tblink;
	link->info = *info;

	ndev = tbrxe_ndev_create(info);
	if (IS_ERR(ndev))
		goto out_free_link;
	link->ndev = ndev;

	rxe = ib_alloc_device(rxe_dev, ib_dev);
	if (!rxe)
		goto out_ndev;

	/* Frame payload budget: eth_mtu_int_to_enum() subtracts the 80-byte
	 * header allowance, so hand it 2048 + 80 to land on IB_MTU_2048.
	 */
	rxe_add(rxe, 2048 + RXE_MAX_HDR_LENGTH, ndev->dev_addr);
	rxe->tbl_link = link;
	link->rxe = rxe;

	err = rxe_register_device(rxe, "usb4_rdma%d", ndev);
	if (err) {
		pr_err("tbrxe: publishing ib_device for link %s failed: %d\n",
		       tbrxe.ops && tbrxe.ops->link_name ?
				tbrxe.ops->link_name(tblink) : "?", err);
		/* dealloc_driver is wired: pools are cleaned up too. */
		ib_dealloc_device(&rxe->ib_dev);
		goto out_ndev;
	}

	link->session_up = true;
	spin_lock_irqsave(&tbrxe.lock, flags);
	list_add_tail(&link->list, &tbrxe.links);
	spin_unlock_irqrestore(&tbrxe.lock, flags);

	dev_info(&rxe->ib_dev.dev,
		 "published for link %s netdev %s peer %s local_eui64=%016llx\n",
		 tbrxe.ops && tbrxe.ops->link_name ?
			tbrxe.ops->link_name(tblink) : "?",
		 netdev_name(ndev),
		 info->remote_name[0] ? info->remote_name : "?",
		 info->local_gid_eui64);
	mutex_unlock(&tbrxe.publish_lock);
	return;

out_ndev:
	unregister_netdev(link->ndev);
	free_netdev(link->ndev);
out_free_link:
	kfree(link);
out_unlock:
	mutex_unlock(&tbrxe.publish_lock);
}

/* Session teardown that unpublishes the device on terminal reasons. */
static void tbrxe_client_link_down(void *ctx, struct tbframe_link *tblink,
				   enum tbframe_down_reason reason)
{
	bool terminal = reason == TBFRAME_DOWN_UNPLUG ||
			reason == TBFRAME_DOWN_CLOSED ||
			reason == TBFRAME_DOWN_DEAD_HW;
	struct tbrxe_link *link;
	unsigned long flags;

	mutex_lock(&tbrxe.publish_lock);

	spin_lock_irqsave(&tbrxe.lock, flags);
	link = tbrxe_find_record_locked(tblink);
	if (link) {
		link->session_up = false;
		link->unacked = 0;
		link->admission_waiters = false;
		if (terminal)
			list_del_init(&link->list);
	}
	spin_unlock_irqrestore(&tbrxe.lock, flags);

	if (!link) {
		mutex_unlock(&tbrxe.publish_lock);
		return;
	}

	if (!terminal) {
		rxe_port_down(link->rxe);
		mutex_unlock(&tbrxe.publish_lock);
		return;
	}

	/* dealloc_driver is wired, so unregister also frees the rxe_dev
	 * once every QP/user is drained; the record must outlive that
	 * drain (engine admission derefs rxe->tbl_link).
	 */
	dev_info(&link->rxe->ib_dev.dev, "unpublished (link down %d)\n",
		 reason);
	ib_unregister_device(&link->rxe->ib_dev);
	unregister_netdev(link->ndev);
	free_netdev(link->ndev);
	kfree(link);

	mutex_unlock(&tbrxe.publish_lock);
}

static const struct tbframe_client_ops tbrxe_client_ops = {
	.rx		= tbrxe_client_rx,
	.tx_released	= tbrxe_client_tx_released,
	.link_up	= tbrxe_client_link_up,
	.link_down	= tbrxe_client_link_down,
};

/* Test hook: the client ops tbrxe registers with tbframe, so KUnit can
 * drive the upcalls directly (there is no tbframe module in that kernel).
 */
const struct tbframe_client_ops *tbrxe_frame_client_ops(void)
{
	return &tbrxe_client_ops;
}

/* ---- lifecycle -------------------------------------------------------- */

int tbrxe_frame_register(void)
{
	int err;

	if (!tbrxe.ops)
		tbrxe.ops = tbrxe_builtin_transport();

	if (tbrxe.ops && tbrxe.ops->register_client) {
		err = tbrxe.ops->register_client(&tbrxe_client_ops, NULL);
		if (err)
			return err;
		tbrxe.registered = true;
	}

	return 0;
}

void tbrxe_frame_unregister(void)
{
	struct tbrxe_link *link, *tmp;

	if (tbrxe.registered && tbrxe.ops && tbrxe.ops->unregister_client) {
		/* Delivers link_down(TBFRAME_DOWN_CLOSED) for every link --
		 * which unpublishes every device -- and returns only when no
		 * upcall can run again.
		 */
		tbrxe.ops->unregister_client();
		tbrxe.registered = false;
	}

	/* Null transport (KUnit without mock teardown): drop leftovers. */
	mutex_lock(&tbrxe.publish_lock);
	list_for_each_entry_safe(link, tmp, &tbrxe.links, list) {
		list_del_init(&link->list);
		ib_unregister_device(&link->rxe->ib_dev);
		unregister_netdev(link->ndev);
		free_netdev(link->ndev);
		kfree(link);
	}
	mutex_unlock(&tbrxe.publish_lock);
}
