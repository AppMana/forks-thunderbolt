// SPDX-License-Identifier: GPL-2.0

#define pr_fmt(fmt) "thunderbolt_ibverbs: " fmt

#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/etherdevice.h>
#include <linux/inetdevice.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/thunderbolt.h>
#include <linux/workqueue.h>

#include "../proto/native_wire.h"
#include "tbv.h"

#define TBV_NATIVE_HELLO_RETRIES 5
#define TBV_NATIVE_TUNNEL_RETRIES 20
#define TBV_NATIVE_READY_RETRIES 10
#define TBV_NATIVE_HELLO_TIMEOUT_MS 1000
#define TBV_NATIVE_HELLO_RETRY_DELAY_MS 200
#define TBV_NATIVE_HELLO_INITIAL_DELAY_MS 100

static void tbv_native_control_work(struct work_struct *work);

static u32 tbv_native_control_caps(const struct tbv_state *state,
				   const struct tbv_peer *peer)
{
	u32 caps = 0;

	if (state->cfg.uc_supported)
		caps |= TBV_NATIVE_WIRE_CAP_UC;
	if (state->cfg.rc_supported)
		caps |= TBV_NATIVE_WIRE_CAP_RC;
	if (peer->nr_rails > 1)
		caps |= TBV_NATIVE_WIRE_CAP_MULTI_RAIL;
	/*
	 * Receive-side support for all four is unconditional in this module:
	 * split-base raw streams, inbound NAKs and inbound absolute credit
	 * resyncs, and negotiated hardware-E2E-only flow control need no
	 * per-node state. The peer gates its own TX on these bits, which matters
	 * for the resync because an older peer rejects the opcode as a bad
	 * header.
	 */
	caps |= TBV_NATIVE_WIRE_CAP_SPLIT_DATA | TBV_NATIVE_WIRE_CAP_NAK |
		TBV_NATIVE_WIRE_CAP_CREDIT_SYNC |
		TBV_NATIVE_WIRE_CAP_E2E_NO_SW_CREDIT;

	return caps;
}

static u32 tbv_native_control_path_flags(const struct tbv_path *path)
{
	u32 flags = 0;

	if (path->cfg.tx_flags & RING_FLAG_FRAME)
		flags |= TBV_NATIVE_WIRE_PATH_FRAME;
	if (path->cfg.e2e)
		flags |= TBV_NATIVE_WIRE_PATH_E2E;

	return flags;
}

/*
 * The local RoCE GID identity advertised in our HELLO (wire v2): the
 * modified-EUI-64 of the roce_netdev MAC (== the interface-id bytes of every
 * MAC-derived GID ib_core builds for our ib_devices) and its first IPv4
 * address (== the IPv4-mapped GID). The receiving peer stores these so it can
 * resolve which Thunderbolt neighbour a destination GID belongs to at
 * modify_qp(RTR) time. Both zero when the netdev does not exist (identity
 * then stays invalid on the remote side).
 */
static void tbv_native_control_local_identity(u64 *eui64, u32 *ipv4_be)
{
	const char *name = tbv_ibdev_roce_netdev_name();
	struct net_device *ndev;
	struct in_device *in_dev;
	const struct in_ifaddr *ifa;
	u8 eui[8];

	*eui64 = 0;
	*ipv4_be = 0;

	if (!name || !*name)
		return;
	ndev = dev_get_by_name(&init_net, name);
	if (!ndev)
		return;

	if (ndev->addr_len == ETH_ALEN) {
		/* RFC 4291 modified EUI-64: mac[0..2] ^ U/L, ff:fe, mac[3..5] */
		eui[0] = ndev->dev_addr[0] ^ 0x02;
		eui[1] = ndev->dev_addr[1];
		eui[2] = ndev->dev_addr[2];
		eui[3] = 0xff;
		eui[4] = 0xfe;
		eui[5] = ndev->dev_addr[3];
		eui[6] = ndev->dev_addr[4];
		eui[7] = ndev->dev_addr[5];
		*eui64 = ((u64)eui[0] << 56) | ((u64)eui[1] << 48) |
			 ((u64)eui[2] << 40) | ((u64)eui[3] << 32) |
			 ((u64)eui[4] << 24) | ((u64)eui[5] << 16) |
			 ((u64)eui[6] << 8) | (u64)eui[7];
	}

	rcu_read_lock();
	in_dev = __in_dev_get_rcu(ndev);
	if (in_dev) {
		ifa = rcu_dereference(in_dev->ifa_list);
		/* canonical numeric form: a.b.c.d -> a<<24|b<<16|c<<8|d */
		if (ifa)
			*ipv4_be = be32_to_cpu(ifa->ifa_address);
	}
	rcu_read_unlock();

	dev_put(ndev);
}

/*
 * 24-bit host identity hash for the rail netdev MAC / RAIL_EUI64 identity,
 * folded from the host router UUID (tb_xdomain local_uuid -- stable across
 * boots, identical for every rail on the host, unique per host). Never
 * returns 0 so an advertised rail identity can never look "unknown"; a NULL
 * uuid (should not happen for a connected XDomain) gets the same constant
 * fallback on every call site, keeping the node self-consistent.
 *
 * Unit-tested: kernel/tests/rail_mac_test.c (run via tools/run-kunit.sh).
 */
u32 tbv_host_identity_hash(const u8 uuid[16])
{
	u32 h = 0;
	int i;

	if (!uuid)
		return 0x544256; /* "TBV" */
	for (i = 0; i < 16; i++)
		h = h * 31 + uuid[i];
	h &= 0xffffffu;
	return h ? h : 0x544256;
}

/*
 * Stable 16-bit link identity for the rail's private GID-only netdev.  A
 * peer_id is allocated from state->next_peer_id each time a service instance
 * is registered, so it is deliberately absent.  The remote router UUID and
 * local Thunderbolt route identify a cable across service teardown/reprobe;
 * rail_id (the symmetric native lane) distinguishes bonded lanes.
 */
u16 tbv_rail_link_identity_hash(const u8 remote_uuid[16], u64 route,
				u32 rail_id)
{
	u32 h = tbv_host_identity_hash(remote_uuid);
	u16 folded;

	h ^= lower_32_bits(route);
	h ^= upper_32_bits(route);
	h ^= rail_id * 0x9e3779b1u;
	h ^= h >> 16;
	h *= 0x7feb352du;
	h ^= h >> 15;
	folded = (u16)(h ^ (h >> 16));
	return folded ? folded : 1;
}

/*
 * Per-rail MAC for the rail's private GID-only netdev:
 * 02:H1:H2:H3:L1:L2 -- locally administered + unicast, H =
 * tbv_host_identity_hash (host-unique), L = the stable link hash above.
 * Every link/lane on a host gets a distinct identity, while service
 * re-registration of that same link retains it.
 *
 * The host part matters: the pre-0.2.35 derivation used the ib node_guid's
 * low 40 bits, whose only variables were the LOCAL peer_id and rail_id -- so
 * every node's "peer 1" rail carried the SAME MAC fleet-wide (02:42:57:52:
 * 42:53 on both appmana-002 and appmana-018), hence the same link-local GID
 * and no value a peer could ever resolve a destination GID against. The
 * RAIL_EUI64 HELLO identity matches on the host part only (eui64 >> 16, the
 * upper 48 bits), because the link hash is private to the remote node.
 *
 * Unit-tested: kernel/tests/rail_mac_test.c (run via tools/run-kunit.sh).
 */
void tbv_rail_netdev_mac(u32 host_hash, u16 link_hash, u8 mac[6])
{
	mac[0] = 0x02;
	mac[1] = (u8)(host_hash >> 16);
	mac[2] = (u8)(host_hash >> 8);
	mac[3] = (u8)host_hash;
	mac[4] = (u8)(link_hash >> 8);
	mac[5] = (u8)link_hash;
}

/*
 * The modified-EUI-64 of a rail netdev MAC (RFC 4291: mac[0..2] ^ U/L bit,
 * ff:fe, mac[3..5]) -- the interface-id bytes of every link-local/SLAAC GID
 * ib_core builds for that rail's ib_device, and the value a RAIL_EUI64 HELLO
 * advertises. Host scope = eui64 >> 16.
 */
u64 tbv_rail_identity_eui64(u32 host_hash, u16 link_hash)
{
	u8 mac[6];

	tbv_rail_netdev_mac(host_hash, link_hash, mac);
	return ((u64)(mac[0] ^ 0x02) << 56) | ((u64)mac[1] << 48) |
	       ((u64)mac[2] << 40) | (0xffULL << 32) | (0xfeULL << 24) |
	       ((u64)mac[3] << 16) | ((u64)mac[4] << 8) | (u64)mac[5];
}

/*
 * True while the roce_netdev exists but has no IPv4 yet (the boot-time DHCP
 * window). A HELLO sent now would advertise ipv4=0, which a peer can never
 * resolve a v4-mapped dgid against.
 */
bool tbv_native_control_local_identity_incomplete(void)
{
	const char *name = tbv_ibdev_roce_netdev_name();
	u64 eui64;
	u32 ipv4;

	if (!name || !*name)
		return false;
	tbv_native_control_local_identity(&eui64, &ipv4);
	if (!eui64 && !ipv4)
		return false; /* netdev absent: identity legitimately empty */
	return !ipv4;
}

static void tbv_native_control_fill_hello(const struct tbv_state *state,
					  const struct tbv_peer *peer,
					  const struct tbv_rail *rail,
					  struct tbv_native_wire_hello *hello)
{
	memset(hello, 0, sizeof(*hello));
	hello->capabilities = tbv_native_control_caps(state, peer);
	hello->rail_id = rail->rail_id;
	hello->route = rail->key.route;
	hello->tx_hop = rail->path.tx_ring ?
			rail->path.tx_ring->hop : U32_MAX;
	hello->rx_hop = rail->path.rx_ring ?
			rail->path.rx_ring->hop : U32_MAX;
	hello->transmit_path = rail->path.local_transmit_path >= 0 ?
			rail->path.local_transmit_path : U32_MAX;
	hello->tx_ring_size = rail->path.cfg.tx_ring_size;
	hello->rx_ring_size = rail->path.cfg.rx_ring_size;
	hello->path_flags = tbv_native_control_path_flags(&rail->path);
	tbv_native_control_local_identity(&hello->roce_eui64,
					  &hello->roce_ipv4);
	/*
	 * No pinned roce_netdev (the ibverbs-native fleet default: the
	 * roce_netdev param is an rxe_lan-era knob and is unset): advertise
	 * the rail netdev's own synthetic identity instead, flagged
	 * RAIL_EUI64 so the peer host-part-matches it. Without this the HELLO
	 * carries (0, 0), the peer never sets remote_identity_valid, and
	 * tbv_qp_rebind_to_dgid() can NEVER resolve a destination GID -- a QP
	 * created on the ib_device of the other neighbour then egresses every
	 * data frame out the wrong rail and the first NCCL exchange hangs
	 * with no WC and no error (appmana-002<->018, 2026-07-11: 018 sent
	 * 578 data frames to route 0x3 while 002 on route 0x1 received none).
	 * KUnit: thunderbolt_ibverbs_qp_first_connect.
	 */
	if (!hello->roce_eui64 && !hello->roce_ipv4 && peer->xd &&
	    peer->xd->local_uuid) {
		hello->roce_eui64 = tbv_rail_identity_eui64(
			tbv_host_identity_hash(peer->xd->local_uuid->b),
			tbv_rail_link_identity_hash(
				peer->xd->remote_uuid ?
					peer->xd->remote_uuid->b : NULL,
				rail->key.route, rail->rail_id));
		hello->capabilities |= TBV_NATIVE_WIRE_CAP_RAIL_EUI64;
	}
}

static bool tbv_native_control_peer_matches_source(
	const struct tbv_peer *peer, const struct tb_xdomain *source_xd)
{
	return !source_xd || peer->xd == source_xd;
}

static bool tbv_native_control_can_kick_rail(const struct tbv_state *state,
					     const struct tbv_rail *rail)
{
	return rail->native_work_state == state &&
	       !READ_ONCE(rail->native_work_stop) &&
	       rail->path.state != TBV_PATH_STOPPED;
}

static int tbv_native_control_kick_matching_rail(
	struct tbv_state *state, const struct tb_xdomain *source_xd,
	const struct tbv_native_wire_info *info, u32 rail_id)
{
	struct tbv_peer *peer;
	int ret = -ENOENT;

	mutex_lock(&state->lock);
	list_for_each_entry(peer, &state->peers, node) {
		struct tbv_rail *rail;

		if (peer->backend != TBV_BACKEND_NATIVE)
			continue;
		if (!tbv_native_control_peer_matches_source(peer, source_xd))
			continue;

		list_for_each_entry(rail, &peer->rails, node) {
			if (rail->key.route != info->route ||
			    rail->rail_id != rail_id)
				continue;

			if (tbv_native_control_can_kick_rail(state, rail)) {
				/*
				 * An inbound HELLO/READY means the peer is
				 * actively (re-)negotiating this rail. Re-arm its
				 * finite retry budgets before re-running the work,
				 * mirroring thunderbolt_net's TBIP_LOGIN handler
				 * (which resets login_retries=0 on an inbound
				 * request). Without this a budget-exhausted rail
				 * ignored a peer that was still trying -- so
				 * convergence depended on our own exhaustion +
				 * re-announce instead of responding to peer
				 * activity. Modelled by tb_test_xdomain_kick_rearm.
				 */
				rail->native_attempts = 0;
				rail->native_tunnel_attempts = 0;
				rail->native_hs.attempts = 0;
				schedule_delayed_work(&rail->native_work, 0);
			}
			ret = 0;
			goto out;
		}
	}

out:
	mutex_unlock(&state->lock);
	return ret;
}

static u8 tbv_native_control_sequence(const struct tbv_rail *rail)
{
	if (rail->path.local_transmit_path >= 0)
		return rail->path.local_transmit_path & 0x3;

	return rail->rail_id & 0x3;
}

void tbv_native_control_init_rail(struct tbv_rail *rail,
				  struct tbv_peer *peer)
{
	rail->peer = peer;
	INIT_DELAYED_WORK(&rail->native_work, tbv_native_control_work);
}

void tbv_native_control_queue_rail(struct tbv_state *state,
				   struct tbv_rail *rail)
{
	rail->native_work_state = state;
	WRITE_ONCE(rail->native_work_stop, false);
	rail->native_attempts = 0;
	rail->native_tunnel_attempts = 0;
	rail->native_last_error = 0;
	rail->native_tunnel_recover = false;
	tb_xdomain_handshake_reset(&rail->native_hs);
	schedule_delayed_work(&rail->native_work,
			      msecs_to_jiffies(TBV_NATIVE_HELLO_INITIAL_DELAY_MS));
}

void tbv_native_control_cancel_rail(struct tbv_rail *rail)
{
	struct tbv_state *state = rail->native_work_state;

	if (state) {
		mutex_lock(&state->lock);
		WRITE_ONCE(rail->native_work_stop, true);
		mutex_unlock(&state->lock);
	} else {
		WRITE_ONCE(rail->native_work_stop, true);
	}
	cancel_delayed_work_sync(&rail->native_work);
	rail->native_work_state = NULL;
}

static bool
tbv_native_control_mark_stall_recovery(struct tbv_rail *rail)
{
	if (!rail || !rail->peer ||
	    rail->peer->backend != TBV_BACKEND_NATIVE ||
	    rail->removing || rail->native_tunnel_recover ||
	    rail->path.state != TBV_PATH_TUNNEL_ENABLED)
		return false;

	rail->native_tunnel_recover = true;
	return true;
}

void tbv_native_control_recover_stalled_rail(struct tbv_rail *rail)
{
	struct tbv_state *state;
	bool queue = false;

	if (!rail || !rail->peer ||
	    rail->peer->backend != TBV_BACKEND_NATIVE)
		return;

	state = READ_ONCE(rail->native_work_state);
	if (!state || READ_ONCE(rail->native_work_stop))
		return;

	mutex_lock(&state->lock);
	if (!READ_ONCE(rail->native_work_stop))
		queue = tbv_native_control_mark_stall_recovery(rail);
	mutex_unlock(&state->lock);

	if (queue)
		mod_delayed_work(system_wq, &rail->native_work, 0);
}

#if IS_ENABLED(CONFIG_KUNIT)
int tbv_test_native_stall_recovery_request(enum tbv_backend_type backend,
					    enum tbv_path_state path_state,
					    bool removing,
					    bool *first_out,
					    bool *second_out,
					    bool *latched_out)
{
	struct tbv_peer peer = {
		.backend = backend,
	};
	struct tbv_rail rail = {
		.peer = &peer,
		.removing = removing,
	};

	rail.path.state = path_state;
	if (first_out)
		*first_out = tbv_native_control_mark_stall_recovery(&rail);
	else
		tbv_native_control_mark_stall_recovery(&rail);
	if (second_out)
		*second_out = tbv_native_control_mark_stall_recovery(&rail);
	else
		tbv_native_control_mark_stall_recovery(&rail);
	if (latched_out)
		*latched_out = rail.native_tunnel_recover;
	return 0;
}
#endif

static int tbv_native_control_snapshot(struct tbv_state *state,
				       const struct tb_xdomain *source_xd,
				       const struct tbv_native_wire_info *info,
				       u32 rail_id,
				       bool require_matching_rail,
				       bool require_tunnel_enabled,
				       struct tbv_native_wire_hello *hello,
				       struct tb_xdomain **xd)
{
	struct tbv_peer *peer;
	int ret = -ENOENT;

	mutex_lock(&state->lock);
	list_for_each_entry(peer, &state->peers, node) {
		struct tbv_rail *rail;

		if (peer->backend != TBV_BACKEND_NATIVE)
			continue;
		if (!tbv_native_control_peer_matches_source(peer, source_xd))
			continue;

		list_for_each_entry(rail, &peer->rails, node) {
			if (rail->key.route != info->route)
				continue;
			if (require_matching_rail && rail->rail_id != rail_id)
				continue;
			if (require_tunnel_enabled &&
			    rail->path.state != TBV_PATH_TUNNEL_ENABLED)
				continue;

			tbv_native_control_fill_hello(state, peer, rail,
						      hello);
			*xd = tb_xdomain_get(peer->xd);
			ret = 0;
			goto out;
		}
	}

out:
	mutex_unlock(&state->lock);
	return ret;
}

static int tbv_native_control_apply_remote(struct tbv_state *state,
					   const struct tb_xdomain *source_xd,
					   const struct tbv_native_wire_info *info,
					   const struct tbv_native_wire_hello *remote,
					   bool require_matching_rail,
					   bool supersede)
{
	struct tbv_peer *peer;
	int ret = -ENOENT;

	/*
	 * Takes state->lock itself so no caller may already hold it.
	 * tbv_native_control_identity_refresh_workfn() does, reaching here as
	 * exchange_once() -> apply_ack() -> apply_remote(), and self-deadlocks
	 * on the HELLO_ACK success path. On a PROVE_LOCKING kernel this names
	 * the offender instead of wedging the work item on the global lock.
	 */
	lockdep_assert_not_held(&state->lock);
	mutex_lock(&state->lock);
	list_for_each_entry(peer, &state->peers, node) {
		struct tbv_rail *rail;

		if (peer->backend != TBV_BACKEND_NATIVE)
			continue;
		if (!tbv_native_control_peer_matches_source(peer, source_xd))
			continue;

		list_for_each_entry(rail, &peer->rails, node) {
			if (rail->key.route != info->route)
				continue;
			if (require_matching_rail &&
			    rail->rail_id != remote->rail_id)
				continue;

			rail->native_negotiated = true;
			rail->remote_rail_id = remote->rail_id;
			rail->remote_transmit_path = remote->transmit_path;
			rail->remote_tx_hop = remote->tx_hop;
			rail->remote_rx_hop = remote->rx_hop;
			/*
			 * Capability bits gate data-plane TX behavior (split
			 * zcopy, NAK). A re-HELLO overwrites them, so a peer
			 * that reloads with an older module downgrades us.
			 */
			WRITE_ONCE(peer->remote_caps, remote->capabilities);
			WRITE_ONCE(rail->path.remote_e2e,
				   !!(remote->path_flags &
				      TBV_NATIVE_WIRE_PATH_E2E));
			tbv_path_set_remote_rx_capacity(&rail->path,
							remote->rx_ring_size);
			/*
			 * Record the peer's RoCE GID identity (wire v2) so
			 * modify_qp(RTR) can resolve destination GIDs to this
			 * peer. Zero means the peer advertised nothing (pre-
			 * 0.2.35 module with no pinned roce_netdev).
			 * RAIL_EUI64 marks a synthetic per-rail identity that
			 * must be matched on its host part only; the host
			 * part is the same for every rail of the peer's node,
			 * so last-HELLO-wins storage is safe even multi-rail.
			 */
			if (remote->roce_eui64 || remote->roce_ipv4) {
				peer->remote_roce_eui64 = remote->roce_eui64;
				peer->remote_roce_ipv4 = remote->roce_ipv4;
				peer->remote_identity_rail_scoped =
					!!(remote->capabilities &
					   TBV_NATIVE_WIRE_CAP_RAIL_EUI64);
				peer->remote_identity_valid = true;
			}
			/*
			 * An inbound HELLO arriving while this rail is already
			 * established means the peer restarted its side without a
			 * link edge (no remove event), so our latched handshake
			 * points at the peer's old rings and our work would
			 * short-circuit instead of re-confirming -- the
			 * re-negotiation hang. Supersede it (shared cross-driver
			 * contract, mirroring thunderbolt_net's LOGOUT reset) so
			 * the kick below re-sends our READY. Only the inbound-HELLO
			 * caller passes supersede; the HELLO_ACK/initiator path
			 * must not self-reset. Reproduced by reconnect_userspace.c
			 * / tbv_test_native_rehello_supersede.
			 */
			if (supersede) {
				/*
				 * If the live tunnel was enabled with a different
				 * out-hop than this HELLO carries, the peer reloaded
				 * with fresh rings: flag a rehop so the work disables
				 * and re-enables the tunnel with the new hop. Without
				 * this the rail re-confirms data-ready into a dead hop
				 * (enable_tunnel() rejects the change with -EBUSY,
				 * path.c:1380). Reproduced by reconnect_userspace.c
				 * scenario 2 / tbv_test_native_rehello_changed_hops.
				 */
				if (rail->path.state == TBV_PATH_TUNNEL_ENABLED &&
				    rail->path.remote_transmit_path !=
					    (int)remote->transmit_path)
					rail->native_tunnel_rehop = true;
				tb_xdomain_handshake_supersede(&rail->native_hs);
			}
			ret = 0;
			goto out;
		}
	}

out:
	mutex_unlock(&state->lock);
	return ret;
}

static int tbv_native_control_apply_ack(struct tbv_state *state,
					const struct tb_xdomain *source_xd,
					const struct tbv_native_wire_info *info,
					const struct tbv_native_wire_hello *remote)
{
	return tbv_native_control_apply_remote(state, source_xd, info, remote,
					       true, false);
}

static int tbv_native_control_mark_remote_ready(struct tbv_state *state,
						const struct tb_xdomain *source_xd,
						const struct tbv_native_wire_info *info,
						const struct tbv_native_wire_hello *remote)
{
	struct tbv_rail *publish = NULL;
	struct tbv_peer *peer;
	int ret = -ENOENT;

	mutex_lock(&state->lock);
	list_for_each_entry(peer, &state->peers, node) {
		struct tbv_rail *rail;

		if (peer->backend != TBV_BACKEND_NATIVE)
			continue;
		if (!tbv_native_control_peer_matches_source(peer, source_xd))
			continue;

		list_for_each_entry(rail, &peer->rails, node) {
			if (rail->key.route != info->route ||
			    rail->rail_id != remote->rail_id)
				continue;

			rail->native_hs.peer_seen = true;
			ret = 0;
			/*
			 * Take a refcount so we can call tbv_ibdev_rail_event
			 * after dropping state->lock — ib_register_device may
			 * sleep and can reach back into our ops.
			 */
			if (tbv_rail_data_ready(rail) && !rail->removing) {
				refcount_inc(&rail->refcnt);
				publish = rail;
			}
			goto out;
		}
	}

out:
	mutex_unlock(&state->lock);
	if (publish) {
		tbv_ibdev_rail_event(state, publish, true);
		tbv_rail_put(publish);
	}
	return ret;
}

static int tbv_native_control_mark_local_ready_sent(
	struct tbv_state *state, const struct tb_xdomain *source_xd,
	const struct tbv_native_wire_info *info, u32 rail_id)
{
	struct tbv_rail *publish = NULL;
	struct tbv_peer *peer;
	int ret = -ENOENT;

	mutex_lock(&state->lock);
	list_for_each_entry(peer, &state->peers, node) {
		struct tbv_rail *rail;

		if (peer->backend != TBV_BACKEND_NATIVE)
			continue;
		if (!tbv_native_control_peer_matches_source(peer, source_xd))
			continue;

		list_for_each_entry(rail, &peer->rails, node) {
			if (rail->key.route != info->route ||
			    rail->rail_id != rail_id)
				continue;

			rail->native_hs.request_sent = true;
			rail->native_last_error = 0;
			ret = 0;
			if (tbv_rail_data_ready(rail) && !rail->removing) {
				refcount_inc(&rail->refcnt);
				publish = rail;
			}
			goto out;
		}
	}

out:
	mutex_unlock(&state->lock);
	if (publish) {
		tbv_ibdev_rail_event(state, publish, true);
		tbv_rail_put(publish);
	}
	return ret;
}

static bool tbv_native_control_rail_data_ready(struct tbv_state *state,
					       const struct tbv_rail *rail)
{
	bool ready;

	mutex_lock(&state->lock);
	ready = tbv_rail_data_ready(rail);
	mutex_unlock(&state->lock);
	return ready;
}

int tbv_native_control_handle_packet(struct tbv_state *state,
				     struct tb_xdomain *source_xd,
				     const void *buf, size_t size)
{
	struct tbv_native_wire_hello remote;
	struct tbv_native_wire_hello local;
	struct tbv_native_wire_info info;
	u8 reply[TBV_NATIVE_WIRE_HELLO_MSG_SIZE];
	struct tb_xdomain *xd = NULL;
	int ret;

	if (!state)
		return 0;

	ret = tbv_native_wire_parse_hello(buf, size, &remote, &info);
	if (ret) {
		/*
		 * If this is a native-wire message of a DIFFERENT version,
		 * scream: the peer's module predates/postdates ours and the
		 * rails will just time out negotiating with no other clue.
		 * (Seen as endless HELLO -110 retries during a mixed-version
		 * rollout.) Anything else is some other protocol's traffic.
		 */
		int ver = tbv_native_wire_peek_version(buf, size);

		if (ver >= 0 && ver != TBV_NATIVE_WIRE_VERSION)
			pr_warn_ratelimited("native control: peer speaks wire v%d, this module speaks v%u -- update thunderbolt-ibverbs on the whole fleet (playbook_thunderbolt_ibverbs.yaml)\n",
					    ver, TBV_NATIVE_WIRE_VERSION);
		return 0;
	}

	if (info.op == TBV_NATIVE_WIRE_OP_HELLO_ACK) {
		ret = tbv_native_control_apply_ack(state, source_xd, &info,
						   &remote);
		if (!ret)
			pr_info("native HELLO_ACK received route=0x%llx rail=0x%x remote_out=%u remote_tx=%u remote_rx=%u\n",
				info.route, remote.rail_id,
				remote.transmit_path, remote.tx_hop,
				remote.rx_hop);
		/*
		 * Let tb_xdomain_request() match the ACK after observing it.
		 * XDomain dispatch calls protocol handlers before request
		 * matching, so consuming the ACK here would make the sender
		 * time out even though the peer replied correctly.
		 */
		return 0;
	}

	if (info.op == TBV_NATIVE_WIRE_OP_READY_ACK)
		return 0;

	if (info.op == TBV_NATIVE_WIRE_OP_READY) {
		memset(&local, 0, sizeof(local));
		ret = tbv_native_control_snapshot(state, source_xd, &info,
						  remote.rail_id, true,
						  false,
						  &local, &xd);
		if (ret) {
			pr_warn_ratelimited("native READY route=0x%llx rail=0x%x has no matching peer rail\n",
					    info.route, remote.rail_id);
			return 1;
		}
		tb_xdomain_put(xd);
		xd = NULL;
		ret = tbv_native_control_snapshot(state, source_xd, &info,
						  remote.rail_id, true,
						  true,
						  &local, &xd);
		if (ret) {
			tbv_native_control_mark_remote_ready(state, source_xd,
							    &info, &remote);
			tbv_native_control_kick_matching_rail(state, source_xd,
							      &info,
							      remote.rail_id);
			return 1;
		}

		ret = tbv_native_control_mark_remote_ready(state, source_xd,
							  &info, &remote);
		if (!ret)
			pr_info("native READY received route=0x%llx rail=0x%x\n",
				info.route, remote.rail_id);

		ret = tbv_native_wire_build_hello(reply, sizeof(reply),
						  &local,
						  TBV_NATIVE_WIRE_OP_READY_ACK,
						  0, info.seq, local.route,
						  info.xdomain_sequence);
		if (ret >= 0)
			ret = tb_xdomain_response(xd, reply, sizeof(reply),
						  TB_CFG_PKG_XDOMAIN_RESP);

		if (ret < 0)
			pr_warn("native READY_ACK route=0x%llx rail=0x%x failed: %d\n",
				info.route, remote.rail_id, ret);
		else
			tbv_native_control_mark_local_ready_sent(state,
								source_xd, &info,
								local.rail_id);

		tb_xdomain_put(xd);
		tbv_native_control_kick_matching_rail(state, source_xd, &info,
						      remote.rail_id);
		return 1;
	}

	if (info.op != TBV_NATIVE_WIRE_OP_HELLO)
		return 0;

	memset(&local, 0, sizeof(local));
	ret = tbv_native_control_snapshot(state, source_xd, &info,
					  remote.rail_id, true, false, &local,
					  &xd);
	if (ret) {
		pr_warn("native HELLO route=0x%llx rail=0x%x has no matching peer rail\n",
			info.route, remote.rail_id);
		return 1;
	}

	ret = tbv_native_control_apply_remote(state, source_xd, &info,
					      &remote, true, true);
	if (!ret)
		pr_info("native HELLO received route=0x%llx rail=0x%x remote_out=%u remote_tx=%u remote_rx=%u\n",
			info.route, remote.rail_id, remote.transmit_path,
			remote.tx_hop, remote.rx_hop);

	ret = tbv_native_wire_build_hello(reply, sizeof(reply), &local,
					  TBV_NATIVE_WIRE_OP_HELLO_ACK,
					  0, info.seq, local.route,
					  info.xdomain_sequence);
	if (ret >= 0)
		ret = tb_xdomain_response(xd, reply, sizeof(reply),
					  TB_CFG_PKG_XDOMAIN_RESP);

	if (ret < 0)
		pr_warn("native HELLO_ACK route=0x%llx failed: %d\n",
			info.route, ret);
	else
		pr_info("native HELLO_ACK route=0x%llx rail=0x%x tx_hop=%u rx_hop=%u out_hop=%u\n",
			info.route, local.rail_id, local.tx_hop,
			local.rx_hop, local.transmit_path);

	tb_xdomain_put(xd);
	tbv_native_control_kick_matching_rail(state, source_xd, &info,
					      remote.rail_id);
	return 1;
}

static int tbv_native_control_exchange_once(struct tbv_state *state,
					    struct tbv_peer *peer,
					    struct tbv_rail *rail,
					    u32 attempt)
{
	struct tbv_native_wire_hello local;
	struct tbv_native_wire_hello remote;
	u8 response[TBV_NATIVE_WIRE_HELLO_MSG_SIZE];
	u8 request[TBV_NATIVE_WIRE_HELLO_MSG_SIZE];
	struct tbv_native_wire_info info;
	int ret;

	if (peer->backend != TBV_BACKEND_NATIVE)
		return 0;

	if (rail->path.state != TBV_PATH_RING_STARTED)
		return -EINVAL;

	/*
	 * tb_xdomain_request() matches responses by XDomain route and protocol
	 * UUID only.  It does not include the XDomain sequence or our native
	 * rail/op fields in the match key, so concurrent native requests over
	 * the same XDomain can steal each other's responses.  Keep one native
	 * control transaction in flight per peer/XDomain.
	 *
	 * Lock order is peer->control_lock outside state->lock, established by
	 * apply_ack() below and by ready_once() -> mark_remote_ready() ->
	 * rail_register_lock -> state->lock. Entering with state->lock held
	 * inverts that and also re-enters it in apply_remote().
	 */
	lockdep_assert_not_held(&state->lock);
	mutex_lock(&peer->control_lock);

	tbv_native_control_fill_hello(state, peer, rail, &local);
	ret = tbv_native_wire_build_hello(request, sizeof(request), &local,
					  TBV_NATIVE_WIRE_OP_HELLO, 0,
					  rail->rail_id, local.route,
					  tbv_native_control_sequence(rail));
	if (ret < 0)
		goto out_unlock;

	memset(response, 0, sizeof(response));
	ret = tb_xdomain_request(peer->xd, request, sizeof(request),
				 TB_CFG_PKG_XDOMAIN_REQ, response,
				 sizeof(response),
				 TB_CFG_PKG_XDOMAIN_RESP,
				 TBV_NATIVE_HELLO_TIMEOUT_MS);
	if (ret)
		goto out_unlock;

	ret = tbv_native_wire_parse_hello(response, sizeof(response), &remote,
					 &info);
	if (ret)
		goto out_unlock;
	if (info.op != TBV_NATIVE_WIRE_OP_HELLO_ACK) {
		ret = -EPROTO;
		goto out_unlock;
	}
	ret = tbv_native_control_apply_ack(state, peer->xd, &info, &remote);
	if (ret)
		goto out_unlock;

	pr_info("native HELLO negotiated route=0x%llx rail=0x%x remote_out=%u remote_tx=%u remote_rx=%u attempt=%u\n",
		info.route, remote.rail_id, remote.transmit_path,
		remote.tx_hop, remote.rx_hop, attempt);
out_unlock:
	mutex_unlock(&peer->control_lock);
	return ret;
}

static int tbv_native_control_ready_once(struct tbv_state *state,
					 struct tbv_peer *peer,
					 struct tbv_rail *rail)
{
	struct tbv_native_wire_hello local;
	struct tbv_native_wire_hello remote;
	u8 response[TBV_NATIVE_WIRE_HELLO_MSG_SIZE];
	u8 request[TBV_NATIVE_WIRE_HELLO_MSG_SIZE];
	struct tbv_native_wire_info info;
	int ret;

	if (peer->backend != TBV_BACKEND_NATIVE)
		return 0;

	if (rail->path.state != TBV_PATH_TUNNEL_ENABLED)
		return -EINVAL;

	/* See tbv_native_control_exchange_once() for why this is serialized. */
	mutex_lock(&peer->control_lock);

	tbv_native_control_fill_hello(state, peer, rail, &local);
	ret = tbv_native_wire_build_hello(request, sizeof(request), &local,
					  TBV_NATIVE_WIRE_OP_READY, 0,
					  rail->rail_id, local.route,
					  tbv_native_control_sequence(rail));
	if (ret < 0)
		goto out_unlock;

	memset(response, 0, sizeof(response));
	ret = tb_xdomain_request(peer->xd, request, sizeof(request),
				 TB_CFG_PKG_XDOMAIN_REQ, response,
				 sizeof(response),
				 TB_CFG_PKG_XDOMAIN_RESP,
				 TBV_NATIVE_HELLO_TIMEOUT_MS);
	if (ret)
		goto out_unlock;

	ret = tbv_native_wire_parse_hello(response, sizeof(response), &remote,
					 &info);
	if (ret)
		goto out_unlock;
	if (info.op != TBV_NATIVE_WIRE_OP_READY_ACK ||
	    remote.rail_id != rail->rail_id) {
		ret = -EPROTO;
		goto out_unlock;
	}

	ret = tbv_native_control_mark_remote_ready(state, peer->xd, &info,
						  &remote);
	if (ret)
		goto out_unlock;

	pr_info("native READY sent route=0x%llx rail=0x%x\n",
		info.route, rail->rail_id);
out_unlock:
	mutex_unlock(&peer->control_lock);
	return ret;
}

static int tbv_native_control_enable_tunnel_once(struct tbv_peer *peer,
						 struct tbv_rail *rail)
{
	int ret;

	/*
	 * Tunnel-enable choke point: when the operator handed the link's data
	 * path to thunderbolt_net (link_owner=tbnet), a peer re-HELLO must not
	 * re-arm our tunnel behind their back. The negotiation itself keeps
	 * running so a runtime handback re-enables without re-probing.
	 */
	if (!tbv_link_owner_rdma_data_allowed(READ_ONCE(peer->state->link_owner)))
		return -EPERM;

	/*
	 * Tunnel activation programs the same XDomain control path as native
	 * HELLO/READY traffic.  Serialize it with those transactions so two
	 * rails on one link do not race Thunderbolt config-space operations.
	 */
	mutex_lock(&peer->control_lock);
	ret = tbv_path_enable_tunnel(&rail->path, peer->xd,
				     rail->remote_transmit_path);
	mutex_unlock(&peer->control_lock);
	return ret;
}

int tbv_native_control_exchange(struct tbv_state *state, struct tbv_peer *peer,
				struct tbv_rail *rail)
{
	u32 attempt;
	int ret = 0;

	for (attempt = 1; attempt <= TBV_NATIVE_HELLO_RETRIES; attempt++) {
		ret = tbv_native_control_exchange_once(state, peer, rail,
						       attempt);
		if (!ret)
			return 0;
		if (ret != -ETIMEDOUT)
			return ret;
		if (attempt < TBV_NATIVE_HELLO_RETRIES)
			msleep(TBV_NATIVE_HELLO_RETRY_DELAY_MS);
	}

	pr_warn("native HELLO route=0x%llx rail=0x%x timed out after %d attempts\n",
		rail->key.route, rail->rail_id, TBV_NATIVE_HELLO_RETRIES);
	return ret;
}

static void tbv_native_control_work(struct work_struct *work)
{
	struct tbv_rail *rail =
		container_of(to_delayed_work(work), struct tbv_rail,
			     native_work);
	struct tbv_state *state = rail->native_work_state;
	struct tbv_peer *peer = rail->peer;
	bool retry = false;
	u32 attempt;
	int ret = 0;

	if (!state || !peer || READ_ONCE(rail->native_work_stop))
		return;

	if (peer->backend != TBV_BACKEND_NATIVE)
		return;

	if (rail->path.state != TBV_PATH_RING_STARTED &&
	    rail->path.state != TBV_PATH_TUNNEL_ENABLED)
		return;

	/*
	 * A native TX ring that posts but never completes is an ICM path which
	 * was acknowledged yet is no longer forwarding. Disconnect it through
	 * the same serialized path used for peer re-hop, then invalidate the
	 * latched negotiation so this pass runs the ordinary HELLO, tunnel and
	 * READY sequence from the beginning. Re-enabling the same ring is not
	 * evidence that it resumed: the path-level recovery-attempt latch remains
	 * set until an actual TX completion, preventing a five-second re-HELLO
	 * loop around the same stranded descriptors.
	 */
	if (READ_ONCE(rail->native_tunnel_recover) &&
	    rail->path.state == TBV_PATH_TUNNEL_ENABLED) {
		mutex_lock(&peer->control_lock);
		ret = tbv_path_disable_tunnel(&rail->path, peer->xd);
		mutex_unlock(&peer->control_lock);
		rail->native_last_error = ret;
		if (ret) {
			retry = true;
			goto out;
		}

		mutex_lock(&state->lock);
		rail->native_tunnel_recover = false;
		rail->native_tunnel_rehop = false;
		rail->native_negotiated = false;
		rail->native_attempts = 0;
		rail->native_tunnel_attempts = 0;
		tb_xdomain_handshake_reset(&rail->native_hs);
		mutex_unlock(&state->lock);
		pr_warn("recovering stalled native tunnel route=0x%llx rail=0x%x through fresh HELLO/READY\n",
			rail->key.route, rail->rail_id);
	}

	if (state->negotiate_native && !rail->native_negotiated) {
		/*
		 * Defer the first HELLO while the local identity is missing
		 * its IPv4 (boot vs DHCP race) so peers never store an
		 * identity they cannot resolve v4-mapped dgids against.
		 * Bounded by identity_grace_until; past it we proceed and the
		 * inetaddr notifier re-HELLOs when the address arrives.
		 */
		if (tbv_native_control_local_identity_incomplete() &&
		    time_before(jiffies, state->identity_grace_until)) {
			retry = true;
			goto out;
		}
		attempt = ++rail->native_attempts;
		ret = tbv_native_control_exchange_once(state, peer, rail,
						       attempt);
		rail->native_last_error = ret;
		if (!ret && tbv_native_control_local_identity_incomplete())
			WRITE_ONCE(state->hello_sent_incomplete, true);
		if (ret) {
			if (attempt < TBV_NATIVE_HELLO_RETRIES) {
				retry = true;
			} else {
				/*
				 * Unanswered HELLOs usually mean the peer's
				 * one-shot XDomain property read raced our
				 * module load and gave up, so it never
				 * recreated our tbverbs service and has no
				 * rail to answer from. Push a fresh
				 * properties-changed (rate-limited) to restart
				 * its read cycle and KEEP retrying instead of
				 * dying: convergence beats the reload roulette
				 * (1-in-10 link survival, 2026-06-12 roll).
				 */
				pr_warn_ratelimited("native HELLO route=0x%llx rail=0x%x still unanswered after %u attempts (%d); re-announcing services and retrying\n",
						    rail->key.route,
						    rail->rail_id, attempt,
						    ret);
				tbv_services_reannounce_native(state);
				rail->native_attempts = 0;
				retry = true;
			}
			goto out;
		}
	}

	/*
	 * Link handed to thunderbolt_net: HELLO negotiation above stays live
	 * (so a runtime handback re-enables without re-probing) but the
	 * tunnel/READY phases are gated -- do not burn tunnel attempts or warn.
	 * A handback queues this work again through tbv_link_owner_work_fn().
	 */
	if (!tbv_link_owner_rdma_data_allowed(READ_ONCE(state->link_owner)))
		goto out;

	/*
	 * Peer reloaded with a different out-hop (flagged by the inbound-HELLO
	 * supersede in tbv_native_control_apply_remote): disable the stale tunnel
	 * back to RING_STARTED so the tunnel phase below re-enables it with the new
	 * remote_transmit_path. Without this the rail re-confirms data-ready into a
	 * dead hop. Reproduced by reconnect_userspace.c scenario 2.
	 */
	if (rail->native_tunnel_rehop &&
	    rail->path.state == TBV_PATH_TUNNEL_ENABLED) {
		mutex_lock(&peer->control_lock);
		ret = tbv_path_disable_tunnel(&rail->path, peer->xd);
		mutex_unlock(&peer->control_lock);
		rail->native_tunnel_rehop = false;
		if (ret) {
			rail->native_last_error = ret;
			retry = true;
			goto out;
		}
	}

	if (state->enable_tunnels &&
	    rail->path.state == TBV_PATH_RING_STARTED) {
		if (!rail->native_negotiated ||
		    rail->remote_transmit_path < 0) {
			ret = -ENOTCONN;
			rail->native_last_error = ret;
			goto out;
		}

		attempt = ++rail->native_tunnel_attempts;
		ret = tbv_native_control_enable_tunnel_once(peer, rail);
		rail->native_last_error = ret;
		if (ret) {
			if (attempt < TBV_NATIVE_TUNNEL_RETRIES) {
				retry = true;
			} else {
				pr_warn("native tunnel route=0x%llx rail=0x%x enable failed after %u attempts: %d\n",
					rail->key.route, rail->rail_id,
					attempt, ret);
			}
			goto out;
		}

		pr_info("enabled tunnel route=0x%llx rail=0x%x out_hop=%d remote_out_hop=%d tx_hop=%d rx_hop=%d\n",
			rail->key.route, rail->rail_id,
			rail->path.local_transmit_path,
			rail->remote_transmit_path,
			rail->path.tx_ring->hop, rail->path.rx_ring->hop);

		/*
		 * Tunnel-enable is the third edge that can complete data-readiness
		 * (alongside local READY sent + remote READY received). If both
		 * READYs already arrived during HELLO retries, neither
		 * mark_local_ready_sent nor mark_remote_ready will fire again —
		 * publish synchronously here so the rail's ib_device appears.
		 */
		mutex_lock(&state->lock);
		if (tbv_rail_data_ready(rail) && !rail->removing) {
			refcount_inc(&rail->refcnt);
			mutex_unlock(&state->lock);
			tbv_ibdev_rail_event(state, rail, true);
			tbv_rail_put(rail);
		} else {
			mutex_unlock(&state->lock);
		}
	}

	if (state->enable_tunnels &&
	    rail->path.state == TBV_PATH_TUNNEL_ENABLED &&
	    !rail->native_hs.request_sent) {
		attempt = ++rail->native_hs.attempts;
		ret = tbv_native_control_ready_once(state, peer, rail);
		if (ret && tbv_native_control_rail_data_ready(state, rail))
			ret = 0;
		rail->native_last_error = ret;
		if (ret) {
			if (attempt < TBV_NATIVE_READY_RETRIES) {
				retry = true;
			} else {
				/*
				 * The READY exchange is the mutual-confirmation
				 * half of the handshake; like the HELLO phase it
				 * must NOT give up after a finite budget. A
				 * coordinated fleet reload exhausts these attempts
				 * while the peer is still settling, and giving up
				 * here strands the rail with the tbverbs service
				 * present but no negotiated peer -- the observed
				 * hang. Re-arm (the tb_xdomain_handshake_reset
				 * contract) and re-announce our services to restart
				 * the peer's property read, then keep retrying until
				 * the handshake completes mutually. Modelled by
				 * tb_test_xdomain_negotiation_hang (forks-thunderbolt).
				 */
				pr_warn_ratelimited("native READY route=0x%llx rail=0x%x unanswered after %u attempts (%d); re-announcing services and retrying\n",
						    rail->key.route,
						    rail->rail_id, attempt, ret);
				tbv_services_reannounce_native(state);
				tb_xdomain_handshake_reset(&rail->native_hs);
				retry = true;
			}
			goto out;
		}
		rail->native_hs.request_sent = true;

		/*
		 * We initiated the READY exchange: ready_once already marked the
		 * remote ready (its READY_ACK carried their state), but that hook
		 * fired before we set native_ready_sent above, so it saw
		 * data_ready=false. Re-check now that all three edges
		 * (tunnel_enabled, ready_sent, remote_ready) are in place and
		 * publish if appropriate. The wire-handler path goes through
		 * mark_local_ready_sent which has the publish baked in; this is
		 * the work-function equivalent.
		 */
		mutex_lock(&state->lock);
		if (tbv_rail_data_ready(rail) && !rail->removing) {
			refcount_inc(&rail->refcnt);
			mutex_unlock(&state->lock);
			tbv_ibdev_rail_event(state, rail, true);
			tbv_rail_put(rail);
		} else {
			mutex_unlock(&state->lock);
		}
	}

out:
	if (retry) {
		mutex_lock(&state->lock);
		if (!READ_ONCE(rail->native_work_stop))
			schedule_delayed_work(&rail->native_work,
					      msecs_to_jiffies(TBV_NATIVE_HELLO_RETRY_DELAY_MS));
		mutex_unlock(&state->lock);
	}
}

int tbv_native_control_start(struct tbv_state *state)
{
	int ret;

	if (!state->cfg.native_enabled)
		return 0;

#ifdef TB_PROTOCOL_HANDLER_HAS_XDOMAIN
	state->native_control_source_aware = true;
	ret = tbv_native_control_xdomain_start(state);
#else
	state->native_control_source_aware = false;
	ret = tbv_native_control_legacy_start(state);
#endif
	if (!ret)
		state->native_control_registered = true;
	return ret;
}

void tbv_native_control_stop(struct tbv_state *state)
{
#ifdef TB_PROTOCOL_HANDLER_HAS_XDOMAIN
	tbv_native_control_xdomain_stop();
#else
	tbv_native_control_legacy_stop();
#endif
	if (state) {
		state->native_control_registered = false;
		state->native_control_source_aware = false;
	}
}

const char *tbv_native_control_mode_name(const struct tbv_state *state)
{
	if (!state || !state->cfg.native_enabled ||
	    !state->native_control_registered)
		return "off";

	return state->native_control_source_aware ? "source_aware" : "legacy";
}

/*
 * Identity refresh: when the roce_netdev gains its first IPv4 after HELLOs
 * already went out with ipv4=0 (boot vs DHCP race past the grace window),
 * re-run the HELLO exchange on every negotiated rail. Peers overwrite the
 * stored identity from the fresh HELLO (apply path), so v4-mapped dgids
 * resolve again without a module reload. 2026-06-13: stale boot identities
 * required a fleet-wide manual reload pass to bring DSV4 up.
 */
void tbv_native_control_identity_refresh_workfn(struct work_struct *work)
{
	struct tbv_state *state =
		container_of(work, struct tbv_state, identity_refresh_work);
	struct tbv_peer *peer;
	int refreshed = 0;

	if (tbv_native_control_local_identity_incomplete())
		return;
	if (!READ_ONCE(state->hello_sent_incomplete))
		return;
	WRITE_ONCE(state->hello_sent_incomplete, false);

	/*
	 * The exchange reaches apply_ack() -> apply_remote(), which takes
	 * state->lock, and it sleeps for up to the HELLO timeout per rail. So
	 * it must run with the lock dropped: snapshot the negotiated rails
	 * under the lock holding a reference on each, then exchange without
	 * it. Rails that go away meanwhile are caught by the removing check.
	 */
	while (true) {
		struct tbv_rail *rail;
		struct tbv_rail *target = NULL;
		struct tbv_peer *target_peer = NULL;

		mutex_lock(&state->lock);
		list_for_each_entry(peer, &state->peers, node) {
			if (peer->backend != TBV_BACKEND_NATIVE)
				continue;
			list_for_each_entry(rail, &peer->rails, node) {
				if (!rail->native_negotiated ||
				    rail->removing ||
				    rail->identity_refreshed)
					continue;
				rail->identity_refreshed = true;
				refcount_inc(&rail->refcnt);
				target = rail;
				target_peer = peer;
				break;
			}
			if (target)
				break;
		}
		mutex_unlock(&state->lock);

		if (!target)
			break;
		if (!tbv_native_control_exchange_once(state, target_peer,
						      target, 1))
			refreshed++;
		tbv_rail_put(target);
	}

	pr_info("identity refresh: re-HELLOed %d negotiated rail(s) after roce_netdev address arrival\n",
		refreshed);
}

static struct tbv_state *tbv_identity_notifier_state;

static int tbv_native_control_inetaddr_event(struct notifier_block *nb,
					     unsigned long event, void *ptr)
{
	struct in_ifaddr *ifa = ptr;
	struct tbv_state *state = READ_ONCE(tbv_identity_notifier_state);
	const char *name = tbv_ibdev_roce_netdev_name();

	if (!state || event != NETDEV_UP || !ifa || !ifa->ifa_dev ||
	    !ifa->ifa_dev->dev)
		return NOTIFY_DONE;
	if (!name || !*name ||
	    strcmp(ifa->ifa_dev->dev->name, name))
		return NOTIFY_DONE;
	if (READ_ONCE(state->hello_sent_incomplete))
		schedule_work(&state->identity_refresh_work);
	return NOTIFY_DONE;
}

static struct notifier_block tbv_native_control_inetaddr_nb = {
	.notifier_call = tbv_native_control_inetaddr_event,
};

int tbv_native_control_identity_notifier_register(struct tbv_state *state)
{
	WRITE_ONCE(tbv_identity_notifier_state, state);
	return register_inetaddr_notifier(&tbv_native_control_inetaddr_nb);
}

void tbv_native_control_identity_notifier_unregister(void)
{
	unregister_inetaddr_notifier(&tbv_native_control_inetaddr_nb);
	WRITE_ONCE(tbv_identity_notifier_state, NULL);
}
