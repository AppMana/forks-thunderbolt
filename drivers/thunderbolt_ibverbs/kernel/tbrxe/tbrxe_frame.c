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
 * The engine currency stays struct sk_buff so that rxe_req.c / rxe_resp.c /
 * rxe_comp.c and the rxe_task machinery are untouched (see "The skb seam"
 * below). Everything netdev/UDP/dst-cache related from rxe_net.c is gone.
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
 *   AV dgid is one of our own GIDs (or multicast, see tbrxe_dest()).
 *
 * RX: one copy from the tbframe RX frame into a fresh skb inside the rx()
 *   upcall, so the frame can be reposted immediately and the engine owns a
 *   normal skb through its req_pkts/resp_pkts queues (upstream rxe also
 *   copies once per direction; holding frames via tbframe_frame_get_rx is a
 *   later optimization).
 *
 * Both RX and loopback skbs carry a synthetic IPv6 header in headroom
 * (saddr = sender GID, daddr = receiver GID, network header set,
 * skb->protocol = ETH_P_IPV6, skb->dev = blackhole_netdev) so the verbatim
 * engine paths that peek at it keep working: the UD GRH copy in
 * rxe_resp.c:execute(), the wc network_hdr_type / is_vlan_dev() code in
 * do_complete(), check_addr() and the mcast dgid extraction in rxe_recv.c.
 *
 * Backpressure: tbframe admission (alloc_frame -> -ENOSPC) replaces the
 * upstream skb-destructor / qp->skb_out scheme. On -ENOSPC the transport
 * sets qp->need_req_skb (the existing flag) and rxe_init_packet returns
 * NULL; rxe_requester treats that as "wait" (the one small edit in
 * rxe_req.c). The tx_released upcall then reschedules every QP's tasks
 * (coalesced, so a full pool sweep is acceptable for the scaffold).
 *
 * Addressing (wire-spec section 8): the target is one ib_device port per
 * tbframe link. The rxe core is deeply single-port (struct rxe_port is
 * scalar, port_num==1 is asserted throughout), so this scaffold uses ONE
 * ib_device with ONE port and resolves "which link reaches this GID" purely
 * through the peer table below; per-link ports are an open item. The local
 * GID table holds a single self GID (index 0) set at the FIRST link_up from
 * info->local_gid_eui64 -- the identity tbframe advertised in OUR OWN HELLO
 * on that link -- so the GID a peer derives from our HELLO and the GID we
 * hand to userspace agree end to end (deriving the self GID from any other
 * source made every remote dgid miss the peer table and parked requesters
 * forever). The ib_device itself is published at that same first link_up
 * (the legacy driver's publish-on-rail-ready), because the identity does
 * not exist before a link comes up. All links share the one identity;
 * per-link local ULAs/ports remain an open item.
 */

#include <linux/skbuff.h>
#include <linux/hash.h>
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

struct tbrxe_link {
	struct list_head	list;
	struct tbframe_link	*tblink;
	struct tbframe_link_info info;
	union ib_gid		peer_gid;
	/* Mode A engine-side admission (wire-spec section 6). All fields
	 * under tbrxe.lock. gen disambiguates a recycled allocation so a
	 * stale qp->tbl (link bounced) never charges the successor link.
	 */
	u64			gen;
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
	struct rxe_dev		*rxe;
	spinlock_t		lock;	/* guards links and ops swaps */
	struct list_head	links;
	union ib_gid		self_gid;
	u64			self_eui64;
	u64			link_gen;
	const struct tbrxe_transport_ops *ops;
	bool			registered;
	/* publication of the ib_device at the first link_up */
	struct mutex		publish_lock;
	bool			published;
} tbrxe = {
	.lock	= __SPIN_LOCK_UNLOCKED(tbrxe.lock),
	.links	= LIST_HEAD_INIT(tbrxe.links),
	.publish_lock = __MUTEX_INITIALIZER(tbrxe.publish_lock),
};

static void tbrxe_client_tx_released(void *ctx, struct tbframe_link *link);

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

/* GID = fd | 56-bit hash(eui64) /64 prefix | eui64 interface id. Both ends
 * derive a peer's GID from that peer's advertised gid_eui64 alone.
 */
void tbrxe_gid_from_eui64(u64 eui64, union ib_gid *gid)
{
	u64 prefix = ((u64)0xfd << 56) | hash_64(eui64, 56);

	gid->global.subnet_prefix = cpu_to_be64(prefix);
	gid->global.interface_id = cpu_to_be64(eui64);
}

int tbrxe_query_gid(struct rxe_dev *rxe, int index, union ib_gid *gid)
{
	/* Single self GID at index 0; the rest of the table is empty. */
	if (index == 0 && rxe == tbrxe.rxe)
		memcpy(gid, &tbrxe.self_gid, sizeof(*gid));
	else
		memset(gid, 0, sizeof(*gid));

	return 0;
}

bool tbrxe_gid_is_local(struct rxe_dev *rxe, const union ib_gid *gid)
{
	return rxe == tbrxe.rxe &&
	       !memcmp(gid, &tbrxe.self_gid, sizeof(*gid));
}

/* Caller holds tbrxe.lock. */
static struct tbrxe_link *tbrxe_find_link_locked(const union ib_gid *dgid)
{
	struct tbrxe_link *link;

	list_for_each_entry(link, &tbrxe.links, list)
		if (!memcmp(&link->peer_gid, dgid, sizeof(*dgid)))
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
 * (a READ advances req.psn by the number of response packets, each of
 * which lands in OUR ring but is symmetric on the peer for its reads).
 * QP reset/error/destroy sync with a zero distance, returning everything.
 *
 * Transient slack: between the pre-charge and update_wqe_psn the charge
 * exceeds the PSN distance by one, so a concurrent completer sync can
 * momentarily release that slot early. The overshoot is bounded by the
 * number of concurrently-sending QPs and absorbed by the control reserve
 * the ring is sized with.
 */

/* Caller holds tbrxe.lock. Validates qp->tbl (pointer, gen) against the
 * live list; resolves from the QP's primary AV when invalid. Returns NULL
 * when the destination is local or no link matches (nothing to charge).
 */
static struct tbrxe_link *tbrxe_qp_link_locked(struct rxe_qp *qp)
{
	struct tbrxe_link *link;

	if (qp->tbl) {
		list_for_each_entry(link, &tbrxe.links, list)
			if (link == qp->tbl && link->gen == qp->tbl_gen)
				return link;
		/* Link bounced: the old charge died with the old link. */
		qp->tbl = NULL;
		qp->tbl_charged = 0;
	}

	link = tbrxe_find_link_locked(
		(const union ib_gid *)&qp->pri_av.grh.dgid);
	if (link) {
		qp->tbl = link;
		qp->tbl_gen = link->gen;
		qp->tbl_charged = 0;
	}
	return link;
}

static u32 tbrxe_link_engine_window(const struct tbrxe_link *link)
{
	return link->info.data_window ? : 1;
}

bool tbrxe_admit(struct rxe_qp *qp)
{
	struct tbrxe_link *link;
	unsigned long flags;
	bool ok = true;

	spin_lock_irqsave(&tbrxe.lock, flags);
	link = tbrxe_qp_link_locked(qp);
	if (link) {
		if (link->unacked >= tbrxe_link_engine_window(link)) {
			link->admission_waiters = true;
			ok = false;
		} else {
			link->unacked++;
			qp->tbl_charged++;
		}
	}
	spin_unlock_irqrestore(&tbrxe.lock, flags);
	return ok;
}

void tbrxe_unacked_sync(struct rxe_qp *qp)
{
	struct tbrxe_link *link;
	unsigned long flags;
	bool kick = false;
	u32 target;

	if (qp_type(qp) != IB_QPT_RC)
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
	link = tbrxe_qp_link_locked(qp);
	if (!link) {
		spin_unlock_irqrestore(&tbrxe.lock, flags);
		return;
	}
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
		tbrxe_client_tx_released(NULL, NULL);
}

/*
 * Local delivery test. Multicast dgids are delivered locally too: the only
 * mcast consumers in the scaffold are self-attached UD groups (rxe_mcast.c);
 * fanning multicast out over tbframe links is an open item.
 */
static bool tbrxe_dest_is_local(const union ib_gid *dgid)
{
	return tbrxe_gid_is_local(tbrxe.rxe, dgid) ||
	       rdma_is_multicast_addr((struct in6_addr *)dgid);
}

/*
 * Synthetic IPv6 header standing in for the GRH, written into headroom so
 * skb->len stays paylen (== BTH..ICRC), matching what rxe_rcv() expects
 * after the upstream UDP pull.
 */
static void tbrxe_fill_grh(struct sk_buff *skb, const union ib_gid *sgid,
			   const union ib_gid *dgid, u16 paylen)
{
	struct ipv6hdr *ip6h;

	skb_set_network_header(skb, -(int)TBRXE_GRH_BYTES);
	ip6h = ipv6_hdr(skb);
	memset(ip6h, 0, sizeof(*ip6h));
	ip6h->version = 6;
	ip6h->payload_len = htons(paylen);
	ip6h->nexthdr = IPPROTO_UDP;
	ip6h->hop_limit = 0xff;
	memcpy(&ip6h->saddr, sgid, sizeof(ip6h->saddr));
	memcpy(&ip6h->daddr, dgid, sizeof(ip6h->daddr));

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
 * pkt->mask and pkt->qp are already valid here for both the request path
 * (rxe_req.c sets them before init_req_packet) and the ack path (the one
 * reordering edit in rxe_resp.c:prepare_ack_packet). ACK-class packets
 * (RXE_ACK_MASK: acknowledges and read responses) are charged to the
 * tbframe control reserve via is_ctrl.
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

	if (tbrxe_dest_is_local((const union ib_gid *)&av->grh.dgid)) {
		skb = alloc_skb(TBRXE_GRH_BYTES + paylen, GFP_ATOMIC);
		if (unlikely(!skb))
			return NULL;

		skb_reserve(skb, TBRXE_GRH_BYTES);
		tbrxe_fill_grh(skb, (union ib_gid *)&av->sgid_addr._sockaddr_in6.sin6_addr,
			       (union ib_gid *)&av->dgid_addr._sockaddr_in6.sin6_addr,
			       paylen);
		pkt->hdr = skb_put(skb, paylen);
	} else {
		spin_lock_irqsave(&tbrxe.lock, flags);
		link = tbrxe_find_link_locked((const union ib_gid *)&av->grh.dgid);
		if (!link || !tbrxe.ops) {
			spin_unlock_irqrestore(&tbrxe.lock, flags);
			return NULL;	/* unreachable: engine errors the QP */
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

/* Downcall 2: address the frame; peer-table lookup replaces routing. */
int rxe_prepare(struct rxe_av *av, struct rxe_pkt_info *pkt,
		struct sk_buff *skb)
{
	struct tbrxe_txinfo *txi;
	unsigned long flags;
	int err = 0;

	if (tbrxe_dest_is_local((const union ib_gid *)&av->grh.dgid)) {
		pkt->mask |= RXE_LOOPBACK_MASK;
		return 0;
	}

	txi = (struct tbrxe_txinfo *)skb->data;

	spin_lock_irqsave(&tbrxe.lock, flags);
	if (!tbrxe_link_is_registered_locked(txi->link) ||
	    tbrxe_find_link_locked((const union ib_gid *)&av->grh.dgid) != txi->link)
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
	struct rxe_dev *rxe = tbrxe.rxe;
	struct tbrxe_link *link = NULL, *l;
	struct rxe_pkt_info *pkt;
	union ib_gid peer_gid;
	struct sk_buff *skb;
	unsigned long flags;
	u16 len = frame->len;

	if (!rxe || frame->pdf != TBFRAME_PDF_DATA)
		return;

	if (len < RXE_BTH_BYTES + RXE_ICRC_SIZE || len > TBFRAME_MAX_FRAME)
		return;

	spin_lock_irqsave(&tbrxe.lock, flags);
	list_for_each_entry(l, &tbrxe.links, list) {
		if (l->tblink == tblink) {
			link = l;
			peer_gid = l->peer_gid;
			break;
		}
	}
	spin_unlock_irqrestore(&tbrxe.lock, flags);
	if (!link)
		return;

	skb = alloc_skb(TBRXE_GRH_BYTES + len, GFP_ATOMIC);
	if (unlikely(!skb))
		return;

	skb_reserve(skb, TBRXE_GRH_BYTES);
	/* daddr: our single self GID (the first link's advertised local
	 * identity); per-link local ULAs are an open item.
	 */
	tbrxe_fill_grh(skb, &peer_gid, &tbrxe.self_gid, len);
	memcpy(skb_put(skb, len), frame->data, len);

	if (!ib_device_try_get(&rxe->ib_dev)) {
		kfree_skb(skb);
		return;
	}

	pkt = SKB_TO_PKT(skb);
	pkt->rxe = rxe;
	pkt->port_num = 1;
	pkt->hdr = skb->data;
	pkt->mask = RXE_GRH_MASK;
	pkt->paylen = len;

	rxe_rcv(skb);
}

/*
 * tx_released(): the link admission window reopened. Coalesced and rare, so
 * a full QP-pool sweep is acceptable for the scaffold: reschedule the send
 * task of every QP parked by -ENOSPC (need_req_skb) and kick recv tasks so
 * stalled read replies / acks retry too.
 */
static void tbrxe_client_tx_released(void *ctx, struct tbframe_link *link)
{
	struct rxe_dev *rxe = tbrxe.rxe;
	struct rxe_pool_elem *elem;
	unsigned long index;
	struct rxe_qp *qp;

	if (!rxe)
		return;

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

static void tbrxe_client_link_up(void *ctx, struct tbframe_link *tblink,
				 const struct tbframe_link_info *info)
{
	struct rxe_dev *rxe = tbrxe.rxe;
	struct tbrxe_link *link;
	unsigned long flags;
	bool first;
	int err;

	if (!rxe)
		return;

	if (info->max_payload < TBRXE_MIN_LINK_PAYLOAD) {
		pr_err("tbrxe: link %s max_payload %u below %u needed for IB_MTU_2048, ignoring link\n",
		       tbrxe.ops && tbrxe.ops->link_name ?
				tbrxe.ops->link_name(tblink) : "?",
		       info->max_payload,
		       (unsigned int)TBRXE_MIN_LINK_PAYLOAD);
		return;
	}

	/*
	 * First link: adopt the identity tbframe advertised in OUR HELLO on
	 * this link as the device self GID, then publish the ib_device
	 * (registration fills the GID cache from rxe_query_gid, so the self
	 * GID must exist before ib_register_device or sgid_index 0 reads
	 * back -ENODATA at RTR). Upcall context is tbframe's workqueue
	 * (process context, no tbframe locks held), so sleeping is fine.
	 */
	mutex_lock(&tbrxe.publish_lock);
	if (!tbrxe.published) {
		tbrxe.self_eui64 = info->local_gid_eui64;
		tbrxe_gid_from_eui64(tbrxe.self_eui64, &tbrxe.self_gid);
		err = tbrxe_publish(rxe);
		if (err) {
			mutex_unlock(&tbrxe.publish_lock);
			pr_err("tbrxe: publishing ib_device at link up failed: %d\n",
			       err);
			return;
		}
		tbrxe.published = true;
	} else if (info->local_gid_eui64 != tbrxe.self_eui64) {
		/* Single shared identity per device for now; a second link
		 * advertising its own per-link ULA cannot be reached through
		 * the published self GID (per-link ports are an open item).
		 */
		pr_warn("tbrxe: link %s advertises local eui64 %016llx but device identity is %016llx; peers on this link will miss\n",
			tbrxe.ops && tbrxe.ops->link_name ?
				tbrxe.ops->link_name(tblink) : "?",
			info->local_gid_eui64, tbrxe.self_eui64);
	}
	mutex_unlock(&tbrxe.publish_lock);

	link = kzalloc(sizeof(*link), GFP_KERNEL);
	if (!link)
		return;

	link->tblink = tblink;
	link->info = *info;
	tbrxe_gid_from_eui64(info->gid_eui64, &link->peer_gid);

	dev_info(&rxe->ib_dev.dev, "link %s up peer_eui64=%016llx local_eui64=%016llx\n",
		 tbrxe.ops && tbrxe.ops->link_name ?
			tbrxe.ops->link_name(tblink) : "?",
		 info->gid_eui64, info->local_gid_eui64);

	spin_lock_irqsave(&tbrxe.lock, flags);
	first = list_empty(&tbrxe.links);
	link->gen = ++tbrxe.link_gen;
	list_add_tail(&link->list, &tbrxe.links);
	spin_unlock_irqrestore(&tbrxe.lock, flags);

	if (first)
		rxe_port_up(rxe);
	/* Peer GIDs live in the peer table only; the local GID table is
	 * unchanged, but poke listeners so caches revalidate.
	 */
	rxe_port_event(rxe, IB_EVENT_GID_CHANGE);
}

static void tbrxe_client_link_down(void *ctx, struct tbframe_link *tblink,
				   enum tbframe_down_reason reason)
{
	struct rxe_dev *rxe = tbrxe.rxe;
	struct tbrxe_link *link = NULL, *l;
	unsigned long flags;
	bool last = false;

	spin_lock_irqsave(&tbrxe.lock, flags);
	list_for_each_entry(l, &tbrxe.links, list) {
		if (l->tblink == tblink) {
			link = l;
			list_del(&l->list);
			last = list_empty(&tbrxe.links);
			break;
		}
	}
	spin_unlock_irqrestore(&tbrxe.lock, flags);

	if (!link)
		return;

	kfree(link);
	if (rxe && last)
		rxe_port_down(rxe);
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

/*
 * Bind the transport to the (not yet published) device. The self identity
 * is NOT derived here: it is the identity tbframe advertises in our own
 * HELLO, which only exists once a link comes up (tbrxe_client_link_up sets
 * the self GID and publishes the ib_device on the first one).
 */
void tbrxe_frame_init(struct rxe_dev *rxe)
{
	tbrxe.rxe = rxe;
}

int tbrxe_frame_register(struct rxe_dev *rxe)
{
	int err;

	if (WARN_ON(tbrxe.rxe != rxe))
		return -EINVAL;

	if (!tbrxe.ops)
		tbrxe.ops = tbrxe_builtin_transport();

	if (tbrxe.ops && tbrxe.ops->register_client) {
		err = tbrxe.ops->register_client(&tbrxe_client_ops, NULL);
		if (err) {
			tbrxe.rxe = NULL;
			return err;
		}
		tbrxe.registered = true;
	}

	return 0;
}

void tbrxe_frame_unregister(void)
{
	struct tbrxe_link *link, *tmp;
	unsigned long flags;

	if (tbrxe.registered && tbrxe.ops && tbrxe.ops->unregister_client) {
		/* Delivers link_down(TBFRAME_DOWN_CLOSED) for every link and
		 * returns only when no upcall can run again.
		 */
		tbrxe.ops->unregister_client();
		tbrxe.registered = false;
	}

	spin_lock_irqsave(&tbrxe.lock, flags);
	list_for_each_entry_safe(link, tmp, &tbrxe.links, list) {
		list_del(&link->list);
		kfree(link);
	}
	tbrxe.rxe = NULL;
	spin_unlock_irqrestore(&tbrxe.lock, flags);
}
