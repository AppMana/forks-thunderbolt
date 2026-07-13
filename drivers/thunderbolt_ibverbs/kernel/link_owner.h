/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Link data-path owner arbitration (usb4_rdma <-> thunderbolt_net handoff).
 *
 * WHAT THE 2026-07 appmana-018<->027 POST-MORTEM ESTABLISHED
 * ----------------------------------------------------------
 * The observed "tbnet passes ZERO packets while usb4_rdma is active" is NOT a
 * static resource conflict. Both drivers' DMA tunnels coexist structurally:
 * separate NHI rings, separate XDomain hopids (both allocated from the same
 * IDAs), separate hop entries, and on TB3 routers both entries even get the
 * same fixed initial credits (tb_dma_reserve_credits() bonded-lane value 14).
 * The hardware read-back on both nodes showed the actual kill mechanism:
 * tbnet's inbound lane-adapter hop entries were DEACTIVATED (enable=0) while
 * tbv's hop 8 entries on the same adapters were enable=1 and carrying heavy
 * RDMA traffic. tbnet's session survives such events as a permanent zombie
 * (carrier on, hop entries dead, both ends latched "login complete") because
 * its ThunderboltIP state machine is one-shot: login_work and connected_work
 * both early-return once carrier is on, and the fleet demonstrably loses the
 * hotplug edges upstream relies on for teardown. thunderbolt_ibverbs survives
 * the identical events because its HELLO negotiation is level-triggered
 * (retries, re-announce, supersede, tunnel re-enable).
 *
 * The durable fix for the zombie itself is therefore in thunderbolt_net
 * (session verify: tb_xdomain_paths_active() polled while carrier is on,
 * tear-down + re-LOGIN when the tunnel died; see tb_xdomain_session_zombie()
 * in thunderbolt_negotiation.h) plus the core read-back helper.
 *
 * WHY AN OWNER SWITCH STILL EXISTS
 * --------------------------------
 * The operator model for the chain remains ONE data-path owner per link at a
 * time: it is deterministic (no cross-traffic interactions to reason about
 * during perf work), and true tbnet+rdma concurrency -- although structurally
 * plausible per the above -- is unvalidated on hardware. This header is the
 * single source of that arbitration: a deterministic, reload-free, RUNTIME
 * handoff. tbv KEEPS thunderbolt_net's ThunderboltIP negotiation completely
 * untouched (spec-compliant "network" service, LOGIN/LOGOUT owned by tbnet)
 * and simply toggles its OWN DMA tunnels:
 *
 *   owner=tbnet: tbv disables its native data tunnels (rings, services,
 *		  negotiation state all stay bound) and unpublishes the
 *		  usb4_rdma ib_devices; thunderbolt_net's session-verify then
 *		  (re)establishes its tunnel and tb-ch* passes packets.
 *   owner=rdma:  tbv re-enables its tunnels through the existing native
 *		  negotiation work and republishes the ib_devices;
 *		  ib_send_lat works again.
 *
 * The toggle is a bounded workqueue apply (no RTNL, no notifiers, no
 * rmmod/insmod, no ICM interaction -- the same tbv_path_disable_tunnel()/
 * enable path the driver already runs on every peer re-HELLO), so it cannot
 * wedge the chain. Idempotent by design (see tbv_link_owner_plan()).
 *
 * Included by the driver (module param toggle + tunnel-enable gate) and the
 * KUnit suite (tests/link_owner_test.c), so there is no drift-prone mirror.
 */
#ifndef _TBV_LINK_OWNER_H
#define _TBV_LINK_OWNER_H

enum tbv_link_owner {
	/* Default: thunderbolt_ibverbs holds the DMA tunnels (RDMA active). */
	TBV_LINK_OWNER_RDMA = 0,
	/* Handoff: tbv releases its tunnels; thunderbolt_net owns the link. */
	TBV_LINK_OWNER_TBNET = 1,
};

enum tbv_link_owner_action {
	TBV_LINK_ACTION_NONE = 0,
	/* Disable tbv DMA tunnels/paths -> hand the link's data path to tbnet. */
	TBV_LINK_ACTION_RELEASE_TBV,
	/* Re-enable tbv DMA tunnels/paths (reclaim the link for RDMA). */
	TBV_LINK_ACTION_CLAIM_TBV,
};

/*
 * May tbv run its DMA data path (enable tunnels) while @owner holds the link?
 * Only when tbv itself is the owner. Consulted by every tunnel-enable choke
 * point (native negotiation work, Apple tunnel enable) so a link handed to
 * tbnet does not re-arm tbv tunnels behind the operator's back when a peer
 * re-HELLOs.
 */
static inline bool tbv_link_owner_rdma_data_allowed(enum tbv_link_owner owner)
{
	return owner == TBV_LINK_OWNER_RDMA;
}

/*
 * Plan the transition to @desired given whether tbv currently holds live DMA
 * tunnels on the link (@tbv_tunnels_live). Idempotent: re-applying an owner
 * whose tunnel state already matches is TBV_LINK_ACTION_NONE, so a repeated
 * write (or a reconcile pass) causes no tunnel churn -- the property that
 * keeps the handoff wedge-free.
 */
static inline enum tbv_link_owner_action
tbv_link_owner_plan(enum tbv_link_owner desired, bool tbv_tunnels_live)
{
	if (tbv_link_owner_rdma_data_allowed(desired))
		return tbv_tunnels_live ? TBV_LINK_ACTION_NONE
					: TBV_LINK_ACTION_CLAIM_TBV;

	return tbv_tunnels_live ? TBV_LINK_ACTION_RELEASE_TBV
				: TBV_LINK_ACTION_NONE;
}

/*
 * The exclusivity POLICY the owner switch enforces (single data-path owner
 * per link). Note this is an operator-model invariant, not the zombie's root
 * cause -- see the header comment. tbnet's ability to pass additionally
 * requires its own session to be live, which its session-verify now
 * guarantees whenever the tunnel underneath it dies.
 */
static inline bool tbv_link_tbnet_can_pass(bool tbv_tunnels_live)
{
	return !tbv_tunnels_live;
}

#endif /* _TBV_LINK_OWNER_H */
