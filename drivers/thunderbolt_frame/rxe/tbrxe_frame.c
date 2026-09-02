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

/* Keep a small part of the advertised data window unavailable to fresh
 * requests.  Replay can use it after a lost ACK, while the separate physical
 * control reserve remains wholly available to ACK/read-response traffic.
 */
#define TBRXE_REPLAY_RESERVE	(TBFRAME_CTRL_RESERVE / 2)
#define TBRXE_QP_CREDIT_CAP	(RXE_MAX_UNACKED_PSNS + \
				 TBRXE_REPLAY_RESERVE)
#define TBRXE_CREDIT_REPLAY	BIT(31)

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
	u32			fresh_unacked;
	u64			credit_generation;
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

/*
 * Unpublish accounting. Unpublishing is asynchronous (see
 * tbrxe_link_unpublish()), so the module needs its own count of devices whose
 * teardown has been queued but whose driver-private state (the record and its
 * GID-anchor netdev, released from rxe_dealloc()) has not been freed yet.
 * tbrxe_frame_drain() is the fence.
 */
static atomic_t tbrxe_unpublishing = ATOMIC_INIT(0);
static DECLARE_WAIT_QUEUE_HEAD(tbrxe_unpublish_waitq);

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
 * normative invariant here: fresh unacknowledged request frames never exceed
 * the peer's advertised data window, total request records including bounded
 * replay never exceed the portion of the physical RX ring reserved for them,
 * and release is correlated with actual peer consumption (the peer reposts
 * the RX descriptor before its engine emits the ACK). No credit messages
 * exist.
 *
 * Accounting: each unique outstanding RC request PSN pre-charges one record
 * in tbrxe_admit(). Successful transmission commits the requester's high-water
 * PSN. ACK/NAK progress releases every record whose PSN precedes comp.psn;
 * reset/error/destroy releases all records. Retry rewind changes req.psn but
 * releases no record, while retransmitting an already-recorded PSN does not
 * charge it twice.
 *
 * Retry state and physical admission are deliberately separate. The frame
 * service recycles an RX descriptor after its client callback returns; RXE
 * copies the frame into an skb during that callback, before the asynchronous
 * responder emits an ACK. A lost ACK therefore does not leave that descriptor
 * occupied, and counting every replay as another occupied descriptor creates
 * a terminal feedback loop once the bounded per-QP ledger fills.
 *
 * A small bounded part of data_window is held back from fresh requests for
 * retransmission (a PSN below the committed high water).  That avoids deadlock
 * when an ACK for a full fresh flight was lost without borrowing from the
 * physical control reserve needed by ACK/read-response traffic.
 *
 * The record outlives every QP of its device (teardown unregisters the
 * ib_device, which drains all QPs, before freeing the record), so
 * rxe->tbl_link is always safe to dereference from engine context.
 *
 * The per-QP record array is bounded by RXE_MAX_UNACKED_PSNS plus the replay
 * reserve.  The engine already prevents a QP from advancing farther than the
 * former, and aggregate admission prevents one QP from owning more replay
 * records than the latter.
 */

static u32 tbrxe_link_engine_window(const struct tbrxe_link *link)
{
	return link->info.data_window ? : 1;
}

static u32 tbrxe_link_replay_reserve(const struct tbrxe_link *link)
{
	u32 data = tbrxe_link_engine_window(link);

	/* Real frame rings are at least 256 entries (data_window >= 192).
	 * Tiny synthetic KUnit windows retain their exact advertised behavior.
	 */
	if (data < 128)
		return 0;
	return min_t(u32, TBRXE_REPLAY_RESERVE, data / 8);
}

static u32 tbrxe_link_fresh_window(const struct tbrxe_link *link)
{
	return tbrxe_link_engine_window(link) -
		tbrxe_link_replay_reserve(link);
}

static u32 tbrxe_link_wire_window(const struct tbrxe_link *link)
{
	return tbrxe_link_engine_window(link);
}

int tbrxe_credit_init(struct rxe_qp *qp)
{
	if (qp_type(qp) != IB_QPT_RC)
		return 0;

	qp->tbl_credits = kcalloc(TBRXE_QP_CREDIT_CAP,
				 sizeof(*qp->tbl_credits), GFP_KERNEL);
	if (!qp->tbl_credits)
		return -ENOMEM;
	qp->tbl_credit_cap = TBRXE_QP_CREDIT_CAP;
	return 0;
}

void tbrxe_credit_cleanup(struct rxe_qp *qp)
{
	kfree(qp->tbl_credits);
	qp->tbl_credits = NULL;
	qp->tbl_credit_count = 0;
	qp->tbl_credit_cap = 0;
}

static void tbrxe_credit_generation_locked(struct rxe_qp *qp,
					    const struct tbrxe_link *link)
{
	if (qp->tbl_generation == link->credit_generation)
		return;

	/* The old session's rings are gone, so none of its records occupies a
	 * descriptor in this generation.  Preserve the PSN high water: a live
	 * RC QP can retry old PSNs after a non-terminal port bounce.
	 */
	qp->tbl_credit_count = 0;
	qp->tbl_generation = link->credit_generation;
}

bool tbrxe_admit(struct rxe_qp *qp, bool *charged)
{
	struct tbrxe_link *link = to_rdev(qp->ibqp.device)->tbl_link;
	unsigned long flags;
	u16 i;
	bool duplicate = false;
	bool replay;
	bool ok = true;

	*charged = false;
	if (!link)
		return true;

	spin_lock_irqsave(&tbrxe.lock, flags);
	tbrxe_credit_generation_locked(qp, link);
	replay = qp->tbl_high_valid &&
		 psn_compare(qp->req.psn, qp->tbl_high_psn) < 0;
	if (replay) {
		for (i = 0; i < qp->tbl_credit_count; i++) {
			if ((qp->tbl_credits[i] & BTH_PSN_MASK) ==
			    (qp->req.psn & BTH_PSN_MASK)) {
				duplicate = true;
				break;
			}
		}
	}
	if (!duplicate &&
	    (link->unacked >= tbrxe_link_wire_window(link) ||
	    (!replay &&
	     link->fresh_unacked >= tbrxe_link_fresh_window(link)) ||
	    qp->tbl_credit_count >= qp->tbl_credit_cap)) {
		/* Park flag BEFORE the waiters flag, under the same lock the
		 * release path takes: a concurrent release that observes
		 * admission_waiters and sweeps is then guaranteed to see
		 * need_req_skb and reschedule this QP (no lost wakeup).
		 */
		qp->need_req_skb = 1;
		link->admission_waiters = true;
		ok = false;
	} else if (!duplicate) {
		link->unacked++;
		if (!replay)
			link->fresh_unacked++;
		qp->tbl_credits[qp->tbl_credit_count++] = qp->req.psn |
			(replay ? TBRXE_CREDIT_REPLAY : 0);
		*charged = true;
	}
	spin_unlock_irqrestore(&tbrxe.lock, flags);
	return ok;
}

/* The requester asks for an immediate cumulative ACK on every replay.  With
 * a bounded replay reserve, waiting for the ordinary periodic ACK cadence can
 * consume the entire reserve before any packet capable of reopening it is
 * emitted.
 */
bool tbrxe_admitted_replay(struct rxe_qp *qp, u32 psn)
{
	struct tbrxe_link *link = to_rdev(qp->ibqp.device)->tbl_link;
	unsigned long flags;
	bool replay = false;

	if (!link || qp_type(qp) != IB_QPT_RC)
		return false;

	spin_lock_irqsave(&tbrxe.lock, flags);
	tbrxe_credit_generation_locked(qp, link);
	if (qp->tbl_high_valid)
		replay = psn_compare(psn, qp->tbl_high_psn) < 0;
	spin_unlock_irqrestore(&tbrxe.lock, flags);

	return replay;
}

/* Roll back the most recent pre-charge when no frame reached the wire. */
void tbrxe_unadmit(struct rxe_qp *qp)
{
	struct tbrxe_link *link = to_rdev(qp->ibqp.device)->tbl_link;
	unsigned long flags;
	u32 credit;

	if (!link || qp_type(qp) != IB_QPT_RC)
		return;

	spin_lock_irqsave(&tbrxe.lock, flags);
	tbrxe_credit_generation_locked(qp, link);
	if (!qp->tbl_credit_count)
		goto out;
	credit = qp->tbl_credits[--qp->tbl_credit_count];
	if (!(credit & TBRXE_CREDIT_REPLAY) && link->fresh_unacked)
		link->fresh_unacked--;
	if (link->unacked)
		link->unacked--;
out:
	spin_unlock_irqrestore(&tbrxe.lock, flags);
}

/* Called after update_wqe_psn(): remember the next never-sent PSN. */
void tbrxe_credit_commit(struct rxe_qp *qp)
{
	struct tbrxe_link *link = to_rdev(qp->ibqp.device)->tbl_link;
	unsigned long flags;

	if (!link || qp_type(qp) != IB_QPT_RC)
		return;

	spin_lock_irqsave(&tbrxe.lock, flags);
	tbrxe_credit_generation_locked(qp, link);
	if (!qp->tbl_high_valid ||
	    psn_compare(qp->req.psn, qp->tbl_high_psn) > 0) {
		qp->tbl_high_psn = qp->req.psn;
		qp->tbl_high_valid = true;
	}
	spin_unlock_irqrestore(&tbrxe.lock, flags);
}

void tbrxe_unacked_sync(struct rxe_qp *qp)
{
	struct tbrxe_link *link;
	struct rxe_dev *rxe;
	unsigned long flags;
	bool released = false;
	bool kick = false;
	u16 src, dst;

	if (qp_type(qp) != IB_QPT_RC)
		return;

	rxe = to_rdev(qp->ibqp.device);
	link = rxe->tbl_link;
	if (!link)
		return;

	spin_lock_irqsave(&tbrxe.lock, flags);
	tbrxe_credit_generation_locked(qp, link);
	for (src = 0, dst = 0; src < qp->tbl_credit_count; src++) {
		u32 credit = qp->tbl_credits[src];
		u32 psn = credit & BTH_PSN_MASK;
		bool release = !qp->valid ||
			qp->attr.qp_state == IB_QPS_ERR ||
			qp->attr.qp_state == IB_QPS_RESET ||
			psn_compare(psn, qp->comp.psn) < 0;

		if (release) {
			if (!(credit & TBRXE_CREDIT_REPLAY) &&
			    link->fresh_unacked)
				link->fresh_unacked--;
			if (link->unacked)
				link->unacked--;
			released = true;
		} else {
			qp->tbl_credits[dst++] = credit;
		}
	}
	qp->tbl_credit_count = dst;
	if (!qp->valid || qp->attr.qp_state == IB_QPS_RESET)
		qp->tbl_high_valid = false;
	if (released && link->admission_waiters) {
		link->admission_waiters = false;
		kick = true;
	}
	spin_unlock_irqrestore(&tbrxe.lock, flags);

	if (kick)
		tbrxe_kick_parked_qps(rxe);
}

#ifdef CONFIG_KUNIT
/* Test observability: the link's live aggregate admission charge. */
u32 tbrxe_link_unacked(struct rxe_dev *rxe)
{
	struct tbrxe_link *link = rxe->tbl_link;
	unsigned long flags;
	u32 val = 0;

	if (!link)
		return 0;

	spin_lock_irqsave(&tbrxe.lock, flags);
	val = link->unacked;
	spin_unlock_irqrestore(&tbrxe.lock, flags);
	return val;
}
#endif

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
	u64	route;
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

/*
 * The link's XDomain route (hex). On an intra-domain self-loop every UUID
 * matches on both ends; the route is the only per-end-distinct value, and
 * the udev naming/addressing tie-breaks (tbv-rdma-ifname, tbv-rdma-addr)
 * read it from here.
 */
static ssize_t tbv_route_show(struct device *d, struct device_attribute *a,
			      char *buf)
{
	struct tbrxe_ndev_priv *priv = netdev_priv(to_net_dev(d));

	return sysfs_emit(buf, "%llx\n", priv->route);
}
static DEVICE_ATTR_RO(tbv_route);

static struct attribute *tbrxe_ndev_attrs[] = {
	&dev_attr_tbv_peer_uuid.attr,
	&dev_attr_tbv_peer_name.attr,
	&dev_attr_tbv_route.attr,
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

/*
 * Build the rail's final interface name, byte-identical to what
 * /usr/lib/usb4-rdma/tbv-rdma-ifname derives for udev:
 *
 *	"tbr-" + last 11 alphanumeric characters of the peer's name
 *
 * ("appmana-019" -> strip non-alnum -> "appmana019" -> "tbr-appmana019".)
 * IFNAMSIZ is 16, and 4 + 11 + NUL is exactly 16.
 *
 * Naming the device here rather than letting udev rename it is what closes
 * the race that stranded rails all of 2026-08-25. tbrxe_link_unpublish()
 * tears the old device down ASYNCHRONOUSLY -- deliberately, because
 * ib_unregister_device_queued() must outlive its userspace users -- so a
 * session that re-establishes promptly creates its replacement while the
 * dying interface still holds the peer name. udev's rename then fails:
 *
 *   u4r0: Failed to rename network interface 61 from 'u4r0' to
 *         'tbr-appmana019': File exists
 *   u4r0: Failed to process device, ignoring: File exists
 *
 * and "ignoring" is the damage: it abandons the whole uevent, including
 * the SEPARATE rule that assigns the rail ULA. The device is left with no
 * address, so ib_core derives no GID, so the ib_device comes up with a
 * zero GID and an unbound netdev -- ACTIVE, and incapable of carrying one
 * byte of RDMA. udev never retries, so the rail stays dead until the next
 * session churn happens to win the race.
 *
 * With the name assigned at alloc time the u4r* rename rule no longer
 * matches at all, so it cannot fail and cannot abort the event; the
 * address rule keys off ATTR{tbv_peer_uuid} and runs regardless.
 */
static void tbrxe_ndev_name(const struct tbframe_link_info *info,
			    char *buf, size_t buflen)
{
	char alnum[IFNAMSIZ];
	size_t n = 0;
	size_t start;
	int i;

	for (i = 0; info->remote_name[i] && n < sizeof(alnum) - 1; i++) {
		if (isalnum(info->remote_name[i]))
			alnum[n++] = info->remote_name[i];
	}
	alnum[n] = '\0';

	/* Last 11 characters, matching the helper's `tail -c 11`. */
	start = n > 11 ? n - 11 : 0;
	snprintf(buf, buflen, "tbr-%s", alnum + start);
}

static struct net_device *tbrxe_ndev_create(const struct tbframe_link_info *info)
{
	struct tbrxe_ndev_priv *priv;
	struct net_device *ndev;
	char name[IFNAMSIZ];
	u8 mac[ETH_ALEN];
	int err;

	tbrxe_ndev_name(info, name, sizeof(name));

	/*
	 * Fall back to the enumerated kernel name only when the peer supplied
	 * nothing usable; udev's helper covers that case from the UUID.
	 */
	if (strlen(name) <= 4)
		strscpy(name, "u4r%d", sizeof(name));

	ndev = alloc_netdev(sizeof(*priv), name, NET_NAME_PREDICTABLE,
			    tbrxe_ndev_setup);
	if (!ndev)
		return ERR_PTR(-ENOMEM);

	priv = netdev_priv(ndev);
	snprintf(priv->peer_uuid, sizeof(priv->peer_uuid), "%pUb",
		 info->remote_uuid);
	strscpy(priv->peer_name, info->remote_name, sizeof(priv->peer_name));
	priv->route = info->route;

	tbrxe_mac_from_eui64(info->local_gid_eui64, mac);
	eth_hw_addr_set(ndev, mac);

	ndev->sysfs_groups[0] = &tbrxe_ndev_group;

	err = register_netdev(ndev);
	if (err == -EEXIST && strcmp(name, "u4r%d")) {
		/*
		 * The previous session's interface has not finished its
		 * asynchronous teardown yet and still owns the peer name.
		 * Take an enumerated name instead of failing the publish:
		 * udev's helper then renames it once the old one is gone,
		 * and -- crucially -- the address rule still runs, so the
		 * rail gets its ULA either way. Loud, because a rail under
		 * the wrong name is invisible to peer-directed tooling.
		 */
		pr_warn("tbrxe: %s still held during re-publish; registering enumerated instead\n",
			name);
		strscpy(ndev->name, "u4r%d", IFNAMSIZ);
		err = register_netdev(ndev);
	}
	if (err) {
		free_netdev(ndev);
		return ERR_PTR(err);
	}
	return ndev;
}

/* ---- publish / unpublish lifecycle ------------------------------------ */

/*
 * Start tearing down one link's device. The caller has already taken the
 * record off tbrxe.links, so no upcall can find it again.
 *
 * ASYNCHRONOUS ON PURPOSE. tbrxe has no .disassociate_ucontext (its user
 * mappings are untracked remap_vmalloc_range() mappings, rxe_mmap.c, which
 * uverbs_user_mmap_disassociate() cannot zap -- same as upstream rxe and
 * siw), so ib_uverbs_remove_one() takes the wait_clients path
 * (drivers/infiniband/core/uverbs_main.c:1254-1284) and a synchronous
 * ib_unregister_device() blocks with no timeout until every userspace
 * process closes its verbs FD. This runs from a tbframe upcall (its
 * workqueue) and from module exit, neither of which may block on userspace,
 * so use the queued form -- the same choice rxe (rxe_net.c:589) and siw
 * (siw_main.c:391) make in their netdev-loss callbacks. It also removes the
 * publish_lock -> rtnl nesting the synchronous form had.
 *
 * The corollary (ib_unregister_device_queued kernel-doc, device.c:1663-1687)
 * is that module exit MUST fence with ib_unregister_driver(); see
 * tbrxe_frame_unregister().
 */
static void tbrxe_link_unpublish(struct tbrxe_link *link)
{
	atomic_inc(&tbrxe_unpublishing);
	ib_unregister_device_queued(&link->rxe->ib_dev);
}

/*
 * dealloc_driver tail (rxe_dealloc()): the last op ib_core calls on the
 * device (ib_dealloc_device(), device.c:690-693), so this is the only point
 * at which the record and its GID-anchor netdev may be freed. Freeing the
 * netdev any earlier would make unregister_netdev() spin waiting for the
 * reference ib_device_set_netdev() holds, which ib_core only drops in
 * free_netdevs() during unregistration (device.c:1566, :707).
 *
 * Also reached for a device that failed registration and is being dropped
 * with ib_dealloc_device(); that path clears rxe->tbl_link first and does
 * its own unwind, so this becomes a no-op there.
 */
void tbrxe_link_release(struct rxe_dev *rxe)
{
	struct tbrxe_link *link = rxe->tbl_link;
	unsigned long flags;

	if (!link)
		return;

	spin_lock_irqsave(&tbrxe.lock, flags);
	rxe->tbl_link = NULL;
	link->rxe = NULL;
	spin_unlock_irqrestore(&tbrxe.lock, flags);

	unregister_netdev(link->ndev);
	free_netdev(link->ndev);
	kfree(link);

	if (atomic_dec_and_test(&tbrxe_unpublishing))
		wake_up(&tbrxe_unpublish_waitq);
}

/*
 * Wait until every queued unpublish has run its dealloc_driver, i.e. until
 * tbrxe owns no per-link state any more. Sleeps; never called from an upcall.
 */
void tbrxe_frame_drain(void)
{
	wait_event(tbrxe_unpublish_waitq, !atomic_read(&tbrxe_unpublishing));
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
		if (err == -ENOSPC && pkt->qp) {
			/* Keep requester and responder backpressure independent.
			 * The release path observes these flags under this lock.
			 */
			if (is_ctrl)
				pkt->qp->need_resp_skb = 1;
			else
				pkt->qp->need_req_skb = 1;
		}
		spin_unlock_irqrestore(&tbrxe.lock, flags);
		if (err)
			return NULL;

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
	if (pkt->mask & RXE_ACK_MASK)
		qp->need_resp_skb = 0;
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

/* IB has no encoding for a 20 Gb/s single lane, so express the link's real
 * aggregate rate as QDR (10 Gb/s) lanes: the product active_speed x
 * active_width is the only thing consumers read, and it lands on the truth.
 *   20 Gb/s x1 -> QDR x 2X = 20000    (Thunderbolt 4, single lane)
 *   20 Gb/s x2 -> QDR x 4X = 40000    (bonded)
 *   10 Gb/s x1 -> QDR x 1X = 10000
 * An unknown or sub-QDR rate keeps the conservative SDR/1X scaffold rather
 * than inventing bandwidth.
 */
void tbrxe_link_ib_rate(u8 tb_speed_gbps, u8 tb_lanes,
			u8 *active_speed, u8 *active_width)
{
	unsigned int lanes = tb_lanes ? tb_lanes : 1;
	unsigned int total = (unsigned int)tb_speed_gbps * lanes;
	unsigned int qdr_lanes = total / 10;

	*active_speed = IB_SPEED_QDR;
	switch (qdr_lanes) {
	case 1:
		*active_width = IB_WIDTH_1X;
		break;
	case 2:
	case 3:
		*active_width = IB_WIDTH_2X;
		break;
	case 4:
	case 5:
	case 6:
	case 7:
		*active_width = IB_WIDTH_4X;
		break;
	case 8:
	case 9:
	case 10:
	case 11:
		*active_width = IB_WIDTH_8X;
		break;
	default:
		if (qdr_lanes >= 12) {
			*active_width = IB_WIDTH_12X;
			break;
		}
		/* Below one QDR lane: keep the scaffold's honest floor. */
		*active_speed = RXE_PORT_ACTIVE_SPEED;
		*active_width = RXE_PORT_ACTIVE_WIDTH;
		break;
	}
}

void tbrxe_rxe_set_link_rate(struct rxe_dev *rxe, u8 tb_speed_gbps, u8 tb_lanes)
{
	u8 speed, width;

	tbrxe_link_ib_rate(tb_speed_gbps, tb_lanes, &speed, &width);

	mutex_lock(&rxe->usdev_lock);
	rxe->port.attr.active_speed = speed;
	rxe->port.attr.active_width = width;
	mutex_unlock(&rxe->usdev_lock);
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

static const char *
tbrxe_bad_frame_icrc_name(enum tbrxe_bad_frame_icrc status)
{
	switch (status) {
	case TBRXE_BAD_FRAME_TOO_SHORT:
		return "too-short";
	case TBRXE_BAD_FRAME_UNSUPPORTED_OPCODE:
		return "unsupported-opcode";
	case TBRXE_BAD_FRAME_ICRC_MATCH:
		return "match";
	case TBRXE_BAD_FRAME_ICRC_MISMATCH:
		return "mismatch";
	}
	return "unknown";
}

/*
 * Diagnostic-only bad-frame path. It never calls rxe_rcv(), looks up a QP or
 * advances requester/responder state. The independent ICRC and BTH identity
 * make the lower NHI descriptor evidence correlatable with RDMA retries.
 */
static void tbrxe_client_rx_bad(void *ctx, struct tbframe_link *tblink,
				struct tbframe_frame *frame)
{
	struct tbrxe_bad_frame_diagnostic diagnostic;
	struct tbrxe_link *link;
	struct rxe_dev *rxe = NULL;
	unsigned long flags;

	tbrxe_bad_frame_diagnose(frame->data, frame->len, &diagnostic);

	spin_lock_irqsave(&tbrxe.lock, flags);
	link = tbrxe_find_record_locked(tblink);
	if (link && ib_device_try_get(&link->rxe->ib_dev))
		rxe = link->rxe;
	spin_unlock_irqrestore(&tbrxe.lock, flags);
	if (!rxe)
		return;

	dev_warn_ratelimited(&rxe->ib_dev.dev,
		"rejected RX payload evidence len=%u pdf=%#x fingerprint=%#010x opcode=%#x qpn=%#x psn=%#x header_len=%u pad=%u software_icrc=%s\n",
		frame->len, frame->pdf, diagnostic.fingerprint,
		diagnostic.opcode, diagnostic.qpn, diagnostic.psn,
		diagnostic.header_len, diagnostic.pad,
		tbrxe_bad_frame_icrc_name(diagnostic.icrc));
	ib_device_put(&rxe->ib_dev);
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
		if (qp->need_resp_skb)
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
		link->fresh_unacked = 0;
		link->credit_generation++;
		link->admission_waiters = false;
	}
	spin_unlock_irqrestore(&tbrxe.lock, flags);
	if (link) {
		/* A re-established session can have renegotiated speed or
		 * width, so refresh the rate before announcing the port
		 * active -- consumers re-read the attributes on the event.
		 */
		tbrxe_rxe_set_link_rate(link->rxe, info->speed, info->width);
		rxe_port_up(link->rxe);
		mutex_unlock(&tbrxe.publish_lock);
		return;
	}

	link = kzalloc(sizeof(*link), GFP_KERNEL);
	if (!link)
		goto out_unlock;
	link->tblink = tblink;
	link->info = *info;
	link->credit_generation = 1;

	ndev = tbrxe_ndev_create(info);
	if (IS_ERR(ndev))
		goto out_free_link;
	link->ndev = ndev;

	rxe = ib_alloc_device(rxe_dev, ib_dev);
	if (!rxe)
		goto out_ndev;

	/* The link's frame payload budget decides the verbs MTU:
	 * a full 4096-byte budget carries the deviated IB_MTU_4096
	 * (engine ceiling TBRXE_MTU4096_PAYLOAD), anything smaller falls
	 * back to the strict IB_MTU_2048 rule (rxe_set_mtu()).
	 */
	rxe_add(rxe, info->max_payload, ndev->dev_addr);
	rxe->tbl_link = link;
	link->rxe = rxe;
	/* Before registration, so the device never appears with the scaffold
	 * rate: a consumer that enumerates on the add event reads the port
	 * attributes once and sizes its topology from them.
	 */
	tbrxe_rxe_set_link_rate(rxe, info->speed, info->width);

	err = rxe_register_device(rxe, "usb4_rdma%d", ndev);
	if (err) {
		pr_err("tbrxe: publishing ib_device for link %s failed: %d\n",
		       tbrxe.ops && tbrxe.ops->link_name ?
				tbrxe.ops->link_name(tblink) : "?", err);
		/*
		 * dealloc_driver is wired, so this cleans the pools and calls
		 * tbrxe_link_release(). Detach the record first: this path
		 * unwinds the netdev and the record itself below, and the
		 * unpublish counter was never incremented for this device.
		 */
		rxe->tbl_link = NULL;
		link->rxe = NULL;
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
		link->fresh_unacked = 0;
		link->credit_generation++;
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

	/*
	 * Queued: this upcall runs on tbframe's workqueue and must not block
	 * on userspace closing verbs FDs. The record and its netdev outlive
	 * the queued teardown and are freed from rxe_dealloc() ->
	 * tbrxe_link_release(), which also keeps rxe->tbl_link valid for the
	 * engine right up to the last QP being drained.
	 */
	dev_info(&link->rxe->ib_dev.dev, "unpublishing (link down %d)\n",
		 reason);
	tbrxe_link_unpublish(link);

	mutex_unlock(&tbrxe.publish_lock);
}

static const struct tbframe_client_ops tbrxe_client_ops = {
	.rx		= tbrxe_client_rx,
	.rx_bad		= tbrxe_client_rx_bad,
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

/*
 * Teardown order (the shape of siw_exit_module(), siw_main.c:490-503):
 *   1. unhook the source of new devices and events,
 *   2. unpublish whatever is left,
 *   3. fence, so nothing of ours is still running or callable.
 */
void tbrxe_frame_unregister(void)
{
	struct tbrxe_link *link;
	unsigned long flags;

	if (tbrxe.registered && tbrxe.ops && tbrxe.ops->unregister_client) {
		/* Delivers link_down(TBFRAME_DOWN_CLOSED) for every link --
		 * which queues an unpublish for every device -- and returns
		 * only when no upcall is running or can run again
		 * (tbframe.h, tbframe_unregister_client()).
		 */
		tbrxe.ops->unregister_client();
		tbrxe.registered = false;
	}

	/* Null/mock transport, or a transport that did not deliver a terminal
	 * link_down for every link: unpublish the leftovers.
	 */
	mutex_lock(&tbrxe.publish_lock);
	for (;;) {
		spin_lock_irqsave(&tbrxe.lock, flags);
		link = list_first_entry_or_null(&tbrxe.links,
						struct tbrxe_link, list);
		if (link)
			list_del_init(&link->list);
		spin_unlock_irqrestore(&tbrxe.lock, flags);
		if (!link)
			break;
		tbrxe_link_unpublish(link);
	}
	mutex_unlock(&tbrxe.publish_lock);

	/* Fence 1: every queued unpublish has reached dealloc_driver, so no
	 * record, netdev or rxe_dev of ours is left.
	 */
	tbrxe_frame_drain();

	/*
	 * Fence 2: ib_core's own module-unload fence, mandatory for any
	 * driver using ib_unregister_device_queued() (device.c:1663-1687) and
	 * the only thing that guarantees no driver op is still callable when
	 * the module text goes away (device.c:695-700). Safe only because
	 * tbrxe owns its driver id now: it sweeps exactly our devices, never
	 * the stock rdma_rxe rail.
	 */
	ib_unregister_driver(RDMA_DRIVER_USB4_RDMA);
}
