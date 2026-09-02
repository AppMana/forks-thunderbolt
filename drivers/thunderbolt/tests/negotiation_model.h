/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Deterministic two-host hardware model of Thunderbolt XDomain re-negotiation,
 * used to reproduce the real appmana-002<->018 stranding and prove the fix.
 *
 * Shared verbatim by the KUnit suite (test.c) and the userspace mirror. Pure C,
 * depends only on the shared negotiation header. Keep copies in lockstep.
 *
 * THERE IS NO "fix" TOGGLE. The only lever is the real production predicate
 * tb_xdomain_generation_stale() (drivers/thunderbolt/xdomain.c:1424). The error
 * emerges from a faithful model of the hardware; commenting out the predicate's
 * fix turns the test red, exactly like reverting the driver.
 *
 * Faithful firmware (Titan Ridge JHL7540 / Maple Ridge JHL8540 disassembly,
 * ../../icm-firmware-re/): ICM_EVENT_XDOMAIN_CONNECTED fires to the host ONLY on
 * the rising edge of physical link-present, latched one-shot, and is
 * best-effort (can be raced/absorbed by a chain retimer). It does NOT re-fire on
 * a property-only change. So when a peer REBOOTS and re-registers its services
 * WITHOUT toggling this host's link edge (or the one-shot event is lost), the
 * ONLY way this host learns of the peer's new property block is a software
 * re-read -- and that re-read is gated by tb_xdomain_generation_stale(). The
 * gate's correctness is therefore load-bearing: there is no firmware fallback.
 *
 * The real bug (fixed 2026-06-16): xdomain_property_block_gen is seeded per boot
 * and a rebooted peer comes back with a generation frequently LOWER than the one
 * a non-rebooted peer still caches. The old `gen <= cached` gate silently
 * dropped every re-read of the lower generation, so the peer's new services were
 * never enumerated -- stranded forever once the best-effort PROPERTIES_CHANGED
 * reset-to-0 was lost. The fixed gate drops only an exact-duplicate re-read, so
 * a lower generation (only possible via a reboot) is accepted and the peer
 * re-enumerates.
 */
#ifndef _TB_NEGOTIATION_MODEL_H
#define _TB_NEGOTIATION_MODEL_H

#include "../thunderbolt_negotiation.h"

/* Faithful ICM: fires once on a rising link edge, never on a property change. */
struct mock_icm {
	bool link_present;
	bool announced;		/* one-shot latch, cleared only on link-down */
};

static inline bool mock_icm_connected_event(struct mock_icm *fw)
{
	if (fw->link_present && !fw->announced) {
		fw->announced = true;
		return true;
	}
	return false;
}

struct model_host {
	/* what we advertise to the peer */
	u32 local_gen;			/* our property block generation */

	/* our enumeration of the peer (persists across the peer's reload) */
	bool have_remote;
	u32 cached_remote_gen;

	struct mock_icm fw;
};

struct model_link {
	struct model_host a, b;
};

/*
 * Core re-read of the peer's property block, gated by the REAL predicate.
 * forced == the firmware-event / PROPERTIES_CHANGED path, which resets the
 * cached generation to 0 first (so any block is accepted). Periodic software
 * re-reads are NOT forced and so are subject to the gate.
 */
static inline void model_core_reread(struct model_host *self,
				     const struct model_host *peer, bool forced)
{
	u32 cached = forced ? 0 : self->cached_remote_gen;

	if (tb_xdomain_generation_stale(self->have_remote, peer->local_gen, cached))
		return;			/* dropped -> keep the stale enumeration */
	self->have_remote = true;
	self->cached_remote_gen = peer->local_gen;
}

/* This host has enumerated the peer's CURRENT services. */
static inline bool model_host_current(const struct model_host *self,
				      const struct model_host *peer)
{
	return self->have_remote && self->cached_remote_gen == peer->local_gen;
}

static inline bool model_established(const struct model_link *L)
{
	return model_host_current(&L->a, &L->b) &&
	       model_host_current(&L->b, &L->a);
}

/* ---- operations ---- */

/* Cold boot: link comes up (rising edge), the firmware event delivers a forced
 * re-read on both sides. */
static inline void model_cold_boot(struct model_link *L, u32 gen_a, u32 gen_b)
{
	L->a.local_gen = gen_a;
	L->b.local_gen = gen_b;
	L->a.fw.link_present = L->b.fw.link_present = true;
	L->a.fw.announced = L->b.fw.announced = false;

	if (mock_icm_connected_event(&L->a.fw))
		model_core_reread(&L->a, &L->b, /*forced=*/true);
	if (mock_icm_connected_event(&L->b.fw))
		model_core_reread(&L->b, &L->a, /*forced=*/true);
}

/*
 * Peer `who` (a or b) reboots and re-registers its services with a NEW
 * generation. Crucially the other host gets NO fresh firmware edge here -- a
 * reboot that the chain retimer absorbs, or whose best-effort one-shot event is
 * lost. So the only recovery is the gated software re-read below.
 */
static inline void model_peer_reboot(struct model_host *who, u32 new_gen)
{
	who->local_gen = new_gen;	/* per-boot reseed; may be lower than cached */
	/* no link edge on the peer's side: its fw.announced stays latched. */
}

/* A round of periodic software re-reads (the state-machine poll). */
static inline void model_poll(struct model_link *L)
{
	model_core_reread(&L->a, &L->b, /*forced=*/false);
	model_core_reread(&L->b, &L->a, /*forced=*/false);
}

static inline void model_run(struct model_link *L, int rounds)
{
	while (rounds-- > 0)
		model_poll(L);
}

/*
 * ---- Co-reset (simultaneous reboot) strand ----
 *
 * Faithful to two facts established from the Maple Ridge disassembly
 * (../../icm-firmware-re/, mr_bank0.c) and the kernel state machine
 * (xdomain.c tb_xdomain_state_work):
 *
 *  (1) Firmware: XDOMAIN_CONNECTED fires once on the rising link edge; the
 *      Maple ICM additionally re-arms that announce ONLY when it receives an
 *      inbound XDP properties-request from the peer. A host that has stopped
 *      its handshake sends no requests, so the peer's ICM never re-arms.
 *
 *  (2) Kernel: XDOMAIN_STATE_PROPERTIES reads the peer with a bounded budget
 *      (XDOMAIN_RETRIES). On exhaustion tb_xdomain_failed() -> XDOMAIN_STATE_ERROR
 *      -> __stop_handshake(): TERMINAL. No periodic re-read survives it.
 *
 * On a SIMULTANEOUS reboot both peers are still booting, so each exhausts its
 * budget before the other can answer, both __stop_handshake, both stop sending
 * requests, neither ICM re-arms -> permanent strand (the live appmana-020<->009
 * failure). A SINGLE reboot recovers: the already-booted peer answers within
 * budget, and the rebooter's request re-arms it.
 *
 * THE LEVER is the real give-up behaviour, kept in lockstep with xdomain.c: a
 * terminal give-up strands the co-reset; re-arming the budget instead recovers.
 */
#define CORESET_RETRY_BUDGET	10	/* == XDOMAIN_RETRIES */

struct coreset_host {
	int  ready_at;		/* round at which this host can answer reads */
	int  retries;		/* remaining property-read retry budget */
	bool stopped;		/* __stop_handshake() reached */
	bool enumerated;	/* read the peer's current properties */
};

struct coreset_link {
	struct coreset_host a, b;
	int round;
};

static inline void coreset_boot(struct coreset_link *L, int ready_a, int ready_b)
{
	memset(L, 0, sizeof(*L));
	L->a.ready_at = ready_a;
	L->b.ready_at = ready_b;
	L->a.retries = L->b.retries = CORESET_RETRY_BUDGET;
}

/*
 * One state-machine tick for `self` reading `peer`, modelling xdomain.c
 * XDOMAIN_STATE_PROPERTIES: reading a peer that has not finished booting fails,
 * the bounded budget decrements, and on exhaustion the host stops the handshake
 * permanently (tb_xdomain_failed -> XDOMAIN_STATE_ERROR -> __stop_handshake).
 * Kept in lockstep with xdomain.c; the fix changes both together.
 */
static inline void coreset_tick(struct coreset_host *self,
				const struct coreset_host *peer, int round)
{
	if (self->stopped || self->enumerated)
		return;
	if (round >= peer->ready_at) {		/* peer answers -> enumerate */
		self->enumerated = true;
		return;
	}
	if (--self->retries <= 0)		/* read failed, budget exhausted */
		self->retries = CORESET_RETRY_BUDGET;	/* re-arm + keep re-reading */
}

static inline bool coreset_run(struct coreset_link *L, int rounds)
{
	for (; rounds-- > 0; L->round++) {
		coreset_tick(&L->a, &L->b, L->round);
		coreset_tick(&L->b, &L->a, L->round);
		if (L->a.enumerated && L->b.enumerated)
			return true;
	}
	return L->a.enumerated && L->b.enumerated;
}

/*
 * ---- Software CM: late host-to-host link enumeration (tb.c) ----
 *
 * Grounded in tb.c: tb_scan_port() enumerates a port's XDomain only if the link
 * is already up (tb_wait_for_port <= 0 -> bail); a link that comes up LATER is
 * re-scanned via tb_handle_hotplug() -> tb_scan_port(). BUT tb_handle_hotplug
 * drops every event while !tcm->hotplug_active (init/suspend/shutdown, the
 * `goto out` near its top). So a host link that trains during the INIT WINDOW --
 * after its initial scan found nothing, before hotplug is armed -- is enumerated
 * by neither path and is lost until something else re-scans.
 *
 * Live: appmana-009 enumerated appmana-025 (port up at boot) but never
 * appmana-020 (its port trained while 009 was still booting), so 009 had no
 * XDomain to answer 020's property requests. Modelled as a per-port state +
 * the arm-ordering; the fix re-scans ports when hotplug is armed.
 */
/*
 * Lane-state samples as tb_reconcile_work() reads them (tb_regs.h
 * enum tb_port_state). Only the three the reconcile logic branches on.
 */
#define CM_SAMPLE_DISABLED	0	/* TB_PORT_DISABLED: lane held down */
#define CM_SAMPLE_CONNECTING	1	/* TB_PORT_CONNECTING: half-trained */
#define CM_SAMPLE_UP		2	/* TB_PORT_UP */
#define CM_SAMPLE_UNPLUGGED	7	/* TB_PORT_UNPLUGGED */

/*
 * Timing constants, in reconcile passes (one pass ~ port_reconcile_ms).
 * Kept in lockstep with tb.c: CM_RETRAIN_QUIET_PASSES mirrors
 * TB_PLUG_RETRAIN_QUIET_MS / TB_RECONCILE_INTERVAL_MS and MUST exceed
 * CM_ICM_BUSY_PASSES -- the safety argument for the plug-directed retrain
 * is exactly "the resident ICM's port state machine settles inside the
 * quiet window, so a bounded edge afterwards cannot race it" (the 05d1e4c
 * wedge class). CM_RETRAIN_MAX_ATTEMPTS mirrors TB_PLUG_RETRAIN_MAX_ATTEMPTS.
 */
#define CM_ICM_BUSY_PASSES	4
#define CM_RETRAIN_QUIET_PASSES	5
#define CM_RETRAIN_HINT_MAX_PASSES 30	/* TB_PLUG_RETRAIN_HINT_MAX_MS */
#define CM_RETRAIN_MAX_ATTEMPTS	3

/*
 * Full-connector software replug (tb_port_connector_replug_*). Mirrors
 * TB_CONNECTOR_REPLUG_HOLD_MS / TB_CONNECTOR_REPLUG_MAX_ATTEMPTS /
 * TB_CONNECTOR_REPLUG_IDLE_RESET_MS in reconcile passes. The hold MUST
 * exceed the ICM busy window: the disable is itself an edge the resident
 * firmware consumes, and the re-enable may only be issued after the
 * firmware has settled (the 2026-08-22 wedge was an edge 3 s after a real
 * unplug -- i.e. inside the busy window that follows the previous edge).
 */
#define CM_REPLUG_HOLD_PASSES	6
#define CM_REPLUG_MAX_ATTEMPTS	2
#define CM_REPLUG_IDLE_RESET_PASSES 300

/* tb_port replug phase (cm_port.replug_phase) */
#define CM_REPLUG_IDLE		0
#define CM_REPLUG_TEARDOWN	1
#define CM_REPLUG_HELD		2

struct cm_port {
	bool link_up;	/* peer's link has trained */
	bool xdomain;	/* this port's XDomain was enumerated */
	/*
	 * The lane-state SAMPLE reads UNPLUGGED even though the link is
	 * electrically up. Observed live 2026-08-20 on appmana-001
	 * (force_sw_cm, resident Titan Ridge ICM): the dock enumerated and
	 * its PCIe tunnel activated, the resident ICM emitted its stray
	 * event (unexpected event 0xa) at t=242, and two seconds later the
	 * root port's LANE_ADP_CS_1 read state 7 while the dock still
	 * answered config space -- the reconcile log line itself said
	 * "router present but lane state 7". CLx (CL0s/CL1 was active on
	 * the link) and plain flaky reads under a coexisting master
	 * produce the same lie.
	 */
	bool state_perturbed;
	/* Lane detection was re-armed by a PHY kick (tb_port_kick_detection). */
	bool detection_rearmed;
	/*
	 * Device attached but the port's detection is LATCHED OFF: the lane
	 * adapter samples UNPLUGGED (state 7) despite a powered peer on a
	 * live cable (the 2026-07-10 019<->008 signature, and the ASM246x
	 * enclosure signature on appmana-001). Only a host lane edge
	 * re-arms it.
	 */
	bool latched_off;
	/*
	 * Lane stuck half-trained at CONNECTING (state 1): present, never
	 * UP. Live 2026-08-22 appmana-001 port 0:4: "failed to reach state
	 * TB_PORT_UP. Ignoring port..." then LANE_ADP_CS_1 parked at state
	 * 1 forever. A fresh training edge completes it (the same disk
	 * enumerated cleanly at 08:26 the same morning).
	 */
	bool half_trained;
	/* Attached hardware that can never train (electrically dead). */
	bool dead;
	/*
	 * The lane trains and samples UP, but the router behind it never
	 * answers config space, so the scan the synthesized plug triggers
	 * finds nothing. This is distinct from ->dead (which never trains)
	 * and from ->latched_off (which samples UNPLUGGED): here the lane
	 * says UP and only the router is mute.
	 */
	bool router_mute;
	/* Synthesized plugs whose follow-up scan found nothing. */
	int plug_synth_failed;
	/*
	 * Passes the resident ICM's port state machine is still
	 * mid-transition after consuming a physical (or host-generated)
	 * lane edge on this port. A host edge issued while this is
	 * nonzero races the firmware and wedges it -- both observed
	 * wedges (2026-08-20 live link, 2026-08-22 3 s post-unplug) are
	 * edges inside this window.
	 */
	int icm_busy;
	int last_sample;	/* previous pass's lane sample */
	int stable;		/* consecutive passes the sample was unchanged */
	int cooldown;		/* passes until post-teardown retrain allowed */
	int since_attempt;	/* passes since the last retrain attempt */
	int attempts;		/* retrain attempts this episode */
	int lane_edges;		/* host-generated edges (boundedness proof) */
	/*
	 * SECONDARY lane adapter (dual_link_port) of this connector. The
	 * live appmana-001 signature (2026-08-22, port 0:4): lane 0 trains
	 * and the router enumerates, but lane 1 parks at CONNECTING --
	 * tb_switch_lane_bonding_enable()'s tb_wait_for_port(dual) prints
	 * "failed to reach state TB_PORT_UP. Ignoring port..." and the
	 * link runs degraded at x1 forever, because NOTHING upstream ever
	 * retries either the lane or the bonding. 0 = no secondary lane
	 * modelled (single-lane tests).
	 */
	int lane1_sample;	/* CM_SAMPLE_* of the secondary lane */
	bool lane1_dead;	/* secondary lane can never train */
	bool bonded;		/* link bonded at x2 */
	int lane1_last;		/* previous pass's secondary sample */
	int lane1_stable;	/* stability of the secondary sample */
	int lane1_since_attempt;
	int lane1_attempts;
	int lane1_edges;	/* host edges on the SECONDARY lane only */
	/*
	 * The secondary lane trains ONLY on a full-connector simultaneous
	 * train: both lanes disabled together, held down, then enabled
	 * together, then a fresh enumeration. Live 2026-08-23 appmana-001
	 * (the same ASM246x/KIOXIA enclosure the lane1 machinery was built
	 * on): 3/3 single-lane bounces with any spacing bounced off (host
	 * lane 1 parked CONNECTING, device lane 1 UNPLUGGED -- the device
	 * arms its lane-1 receiver only during the connector handshake),
	 * device-side DPR/SLI/PE pokes all inert, while both-lanes-down
	 * 12 s + both-up + rescan trained the link to bonded x2 20 Gb/s.
	 * Yesterday's "marginal cable seat, only a physical replug
	 * recovers" conclusion is retired by that run: the replug's
	 * essential ingredient is the full-connector electrical train,
	 * which software CAN issue.
	 */
	bool lane1_full_train_only;
	bool lanes_held;	/* both lane adapters held disabled */
	int replug_phase;	/* CM_REPLUG_* */
	int replug_held;	/* passes both lanes have been held down */
	int replug_attempts;
	int replug_since_attempt;
	int connector_edges;	/* full-connector (both-lane) edges */
};

struct cm_host {
	struct cm_port port[2];
	bool hotplug_armed;
	/*
	 * A broken ICM firmware is left resident and coexists with the
	 * software CM (force_sw_cm). It consumes every hardware edge the
	 * host generates on the lanes, racing its own port state machine.
	 */
	bool resident_icm;
	/*
	 * The resident firmware wedged on a host-generated lane edge.
	 * 2026-08-20 appmana-001: kick on a live link -> control path hung.
	 * 2026-08-22 appmana-001: kick after a REAL enclosure unplug ->
	 * silent NHI MMIO-stall freeze three seconds later (journal stops
	 * mid-line, no hung-task output, hard reset). Recovery requires
	 * power removal (S5), not a warm reboot.
	 */
	bool icm_wedged;
	/*
	 * The resident ICM's own hot-event notification (TB_CFG_PKG_ICM_EVENT,
	 * the live "unexpected event 0xa"): under force_sw_cm the firmware
	 * still watches the ports and tells us about plug edges the latched
	 * -off lane adapter never shows. 0 = none; set to 1 when the event
	 * fires, then ages one per reconcile pass. This is the CONNECT-
	 * REQUEST signal that makes the state-7 retrain plug-DIRECTED
	 * instead of blind.
	 */
	int hint_age;
};

/* Initial scan of one port: enumerate it only if the link is already up. */
static inline void cm_scan_port(struct cm_host *h, int p)
{
	if (h->port[p].link_up)
		h->port[p].xdomain = true;
}

/*
 * A peer finishes booting and its link to port @p trains. Models the resulting
 * plug event: handled (re-scan) only if hotplug is armed, else dropped.
 */
static inline void cm_link_up(struct cm_host *h, int p)
{
	h->port[p].link_up = true;
	if (h->hotplug_armed)
		h->port[p].xdomain = true;	/* tb_handle_hotplug -> tb_scan_port */
	/* else: !tcm->hotplug_active -> event dropped */
}

/*
 * Arm hotplug, then re-scan every port (tb_start() calls tb_scan_switch() right
 * after setting hotplug_active) so a link that trained during the init window is
 * caught. Kept in lockstep with tb.c; reverting the re-scan strands the late
 * link again.
 */
static inline void cm_arm_hotplug(struct cm_host *h)
{
	int i;

	h->hotplug_armed = true;
	for (i = 0; i < 2; i++) {
		/* No retrain has ever run (tb_port's zeroed jiffies stamp
		 * reads as "long ago", not "just now"). */
		h->port[i].since_attempt = CM_RETRAIN_HINT_MAX_PASSES + 1;
		h->port[i].lane1_since_attempt = CM_RETRAIN_HINT_MAX_PASSES + 1;
		h->port[i].replug_since_attempt = CM_REPLUG_IDLE_RESET_PASSES + 1;
		cm_scan_port(h, i);
	}
}

/*
 * ---- Software CM: LOST hot events (tb.c reconciliation) ----
 *
 * Live evidence (2026-07-09/10, appmana-019<->008): when 019 rebooted at 17:50,
 * the survivor 008 received NO unplug and NO plug event for its port 3 -- its
 * kernel log is silent across the whole window while the port's lane adapter
 * reads UNPLUGGED (LANE_ADP_CS_1 state 0x7). The stale XDomain (and its
 * usb4_rdma ib_device) stayed registered for 12 hours; NCCL hung instead of
 * failing. The mirror case also occurs: a link trains but the plug event is
 * absorbed (the reboot-cover script exists because "module reloads race the
 * neighbour's one-shot property re-read and usually lose").
 *
 * tb_handle_hotplug() is edge-triggered ONLY: if the router's hot event is
 * lost (wedged port state machine, ctl ring hiccup, event raced during
 * init/resume), the software topology diverges from the hardware lane state
 * FOREVER. There is no level-triggered fallback.
 *
 * cm_link_up_lost()/cm_link_down_lost() model the lost edges. cm_reconcile()
 * is kept in LOCKSTEP with tb.c: it models what the driver does to converge
 * software topology to hardware lane state without an event. While tb.c has
 * no reconciliation, cm_reconcile() must stay empty and the recovery tests go
 * red -- exactly like reverting the driver.
 */
/*
 * The resident ICM consumes a physical lane edge on port @p: its port state
 * machine goes busy for a while and it emits its ICM-protocol notification
 * (the "unexpected event 0xa" the software CM sees). No-op on a chain host
 * with no resident firmware.
 */
static inline void cm_icm_notices_edge(struct cm_host *h, int p)
{
	if (!h->resident_icm)
		return;
	h->port[p].icm_busy = CM_ICM_BUSY_PASSES;
	h->hint_age = 1;
}

static inline void cm_link_up_lost(struct cm_host *h, int p)
{
	h->port[p].link_up = true;	/* plug event lost: no scan happens */
	cm_icm_notices_edge(h, p);
}

static inline void cm_link_down_lost(struct cm_host *h, int p)
{
	h->port[p].link_up = false;	/* unplug event lost: xdomain stays */
	cm_icm_notices_edge(h, p);
}

/*
 * An enclosure is attached to a port whose detection is latched off: the
 * lane sample never changes (still UNPLUGGED), so the ONLY plug signal is
 * the resident ICM's notification. On a chain host there is no signal at
 * all here -- that case is what the start-time blind kick covers.
 */
static inline void cm_enclosure_attach_latched(struct cm_host *h, int p)
{
	h->port[p].latched_off = true;
	cm_icm_notices_edge(h, p);
}

/*
 * An enclosure is attached and its link half-trains: the lane parks at
 * CONNECTING (the live appmana-001 port 0:4 signature). @dead models
 * hardware that can never complete training no matter how many fresh
 * edges it is given.
 */
static inline void cm_enclosure_attach_marginal(struct cm_host *h, int p,
						bool dead)
{
	h->port[p].half_trained = true;
	h->port[p].dead = dead;
	cm_icm_notices_edge(h, p);
}

/*
 * An enclosure is attached, its PRIMARY lane trains and the router
 * enumerates -- but the SECONDARY lane parks at CONNECTING, so lane
 * bonding fails at enumeration and the link runs degraded x1 (the live
 * appmana-001 0-3/0:4 signature). @dead: the secondary lane can never
 * train no matter how many edges it gets.
 */
static inline void cm_enclosure_attach_bonding_stuck(struct cm_host *h, int p,
						     bool dead)
{
	h->port[p].link_up = true;
	h->port[p].lane1_sample = CM_SAMPLE_CONNECTING;
	h->port[p].lane1_dead = dead;
	cm_icm_notices_edge(h, p);
}

/*
 * The live 2026-08-23 refinement of the signature above: the secondary
 * lane is NOT dead -- it trains on a full-connector simultaneous train
 * (physical replug, or the software replug) -- but no single-lane edge
 * ever wakes it. @dead additionally models a secondary that not even a
 * full-connector train recovers.
 */
static inline void cm_enclosure_attach_bonding_stuck_full_train(struct cm_host *h,
								int p, bool dead)
{
	h->port[p].link_up = true;
	h->port[p].lane1_sample = CM_SAMPLE_CONNECTING;
	h->port[p].lane1_full_train_only = true;
	h->port[p].lane1_dead = dead;
	cm_icm_notices_edge(h, p);
}

/*
 * What tb_reconcile_work() actually observes: one lane-state register read.
 * A dead peer reads UNPLUGGED -- but so does a live link whose state was
 * perturbed by a coexisting master or a CLx transition (state_perturbed),
 * a latched-off port with a powered peer, and a half-trained lane reads
 * CONNECTING: neither present nor gone.
 */
static inline int cm_lane_sample(const struct cm_host *h, int p)
{
	const struct cm_port *port = &h->port[p];

	if (port->lanes_held)
		return CM_SAMPLE_DISABLED;
	if (port->state_perturbed || port->latched_off)
		return CM_SAMPLE_UNPLUGGED;
	if (port->link_up)
		return CM_SAMPLE_UP;
	if (port->half_trained)
		return CM_SAMPLE_CONNECTING;
	return CM_SAMPLE_UNPLUGGED;
}

static inline bool cm_lane_sample_unplugged(const struct cm_host *h, int p)
{
	return cm_lane_sample(h, p) == CM_SAMPLE_UNPLUGGED;
}

/*
 * Bounded config-space probe of the enumerated child/peer router. A dead
 * peer's router is unpowered and cannot answer; a live one answers no
 * matter what its lane-state register momentarily reads.
 */
static inline bool cm_peer_probe(const struct cm_host *h, int p)
{
	return h->port[p].link_up && !h->port[p].router_mute;
}

/*
 * A peer whose lane trains but whose router never answers config space.
 * The lane sample reads UP, so the reconcile synthesizes a plug; the scan
 * that plug triggers then finds nothing, and the port stays unenumerated.
 */
static inline void cm_peer_attach_router_mute(struct cm_host *h, int p)
{
	h->port[p].link_up = true;
	h->port[p].router_mute = true;
	cm_icm_notices_edge(h, p);
}

/*
 * Hardware model of a host-generated lane edge (the lane disable/enable
 * bounce of tb_port_kick_detection() / tb_port_plug_retrain()). On a host
 * with no resident firmware it re-arms latched-off detection (the 019<->008
 * segment) or gives a half-trained lane a fresh training start. On a host
 * whose broken ICM is left resident the firmware consumes the edge too:
 * an edge issued while the firmware's port state machine is BUSY with a
 * transition of its own -- a live link it tracks (2026-08-20: kick on a
 * live link -> control path hung) or the window right after a physical
 * edge it is still processing (2026-08-22: kick 3 s after a REAL enclosure
 * unplug -> silent NHI MMIO-stall freeze) -- races it and wedges. An edge
 * on a QUIESCENT port is the same recipe the ICM's own init runs
 * (icm_reset_phy_port()); the firmware consumes it and starts a fresh
 * detection/training cycle of its own, going busy again.
 */
static inline void cm_hw_lane_edge(struct cm_host *h, int p)
{
	struct cm_port *port = &h->port[p];

	port->lane_edges++;
	if (h->resident_icm && (port->link_up || port->icm_busy > 0)) {
		h->icm_wedged = true;
		return;
	}
	port->detection_rearmed = true;
	if (port->latched_off) {
		port->latched_off = false;
		if (!port->dead)
			port->link_up = true;
	} else if (port->half_trained && !port->dead) {
		port->half_trained = false;
		port->link_up = true;
	}
	/*
	 * The firmware consumes our edge and goes busy again, and it DOES
	 * announce the edge back to us -- but the driver self-attributes a
	 * notification arriving right after its own retrain
	 * (TB_PLUG_RETRAIN_SELF_MS) and never arms it as a hint. Arming it
	 * here turns the bounded retrain into a self-sustaining bounce
	 * loop: each edge seeds the hint for the next (caught by
	 * tb_test_cm_plug_retrain_post_unplug_cooldown).
	 */
	if (h->resident_icm)
		h->port[p].icm_busy = CM_ICM_BUSY_PASSES;
}

/*
 * Lockstep with tb.c tb_port_kick_detection(): kick only an UNPLUGGED port,
 * and NEVER under a resident ICM (the tb_force_sw_cm bail is the fix --
 * removing it here like removing it in the driver wedges the resident-ICM
 * host and the recovery test goes red). Chain hosts run the software CM
 * natively: nothing else consumes the edge and the kick stays.
 */
static inline void cm_kick_detection(struct cm_host *h, int p)
{
	if (h->resident_icm)
		return;
	if (!cm_lane_sample_unplugged(h, p))
		return;
	cm_hw_lane_edge(h, p);
}

/*
 * Lockstep with tb.c tb_reconcile_work(): poll each null port's lane state
 * (tb_port_state) and synthesize the missing hotplug edge so the software
 * topology converges to the hardware:
 *   - xdomain present, lane sample UNPLUGGED AND the bounded config-space
 *     probe of the enumerated peer fails (tb_reconcile_peer_reachable())
 *                                               -> synthesized unplug
 *   - lane UP but nothing enumerated            -> synthesized plug
 * One lane-state sample alone is NOT evidence of an unplug: a coexisting
 * master (resident ICM) or a CLx transition perturbs the register on a
 * live link (2026-08-20 appmana-001 live-tunnel teardown). Runs only
 * while hotplug is armed (same tcm->hotplug_active gate).
 */
/*
 * Lockstep with tb.c tb_port_plug_retrain(): the bounded, plug-DIRECTED
 * retrain that replaces the blind kick under a resident ICM. Preconditions
 * mirror the driver predicates one to one:
 *   - the lane sample has been stable for the whole quiet window (longer
 *     than the ICM's busy window, so the edge cannot race the firmware);
 *   - not inside the post-teardown cooldown (the 2026-08-22 wedge window);
 *   - attempts are spaced at least a quiet window apart and capped per
 *     episode (marginal hardware gets a bounded number of edges, then the
 *     port is left alone until it enumerates or goes idle);
 *   - a lane parked at CONNECTING is itself the presence signal; a lane
 *     sampling UNPLUGGED is bounced ONLY on the resident ICM's own aged
 *     connect notification (one attempt per notification).
 */
static inline void cm_plug_retrain(struct cm_host *h, int p, int sample)
{
	struct cm_port *port = &h->port[p];

	if (port->stable < CM_RETRAIN_QUIET_PASSES)
		return;
	if (port->cooldown > 0)
		return;
	if (port->since_attempt < CM_RETRAIN_QUIET_PASSES)
		return;
	if (sample == CM_SAMPLE_UNPLUGGED) {
		if (h->hint_age <= CM_RETRAIN_QUIET_PASSES ||
		    h->hint_age > CM_RETRAIN_HINT_MAX_PASSES)
			return;
		if (port->since_attempt < h->hint_age)
			return;		/* one attempt per notification */
	}
	if (port->attempts >= CM_RETRAIN_MAX_ATTEMPTS)
		return;
	port->attempts++;
	port->since_attempt = 0;
	cm_hw_lane_edge(h, p);
}

/*
 * Host edge on the SECONDARY lane only; the primary (carrying the
 * enumerated router and its tunnels) is never touched. Wedge condition
 * under a resident ICM: only an in-flight firmware transition on the
 * connector (icm_busy) -- the settled-live-primary case is exactly the
 * regime the lane-directed retrain is allowed to touch, verified live
 * (a full-connector bounce of a live link is the 2026-08-20 wedge and
 * remains forbidden: nothing ever calls this on the primary).
 */
static inline void cm_hw_lane1_edge(struct cm_host *h, int p)
{
	struct cm_port *port = &h->port[p];

	port->lane1_edges++;
	if (h->resident_icm && port->icm_busy > 0) {
		h->icm_wedged = true;
		return;
	}
	if (!port->lane1_dead && !port->lane1_full_train_only)
		port->lane1_sample = CM_SAMPLE_UP;
	if (h->resident_icm)
		port->icm_busy = CM_ICM_BUSY_PASSES;
}

/*
 * Hardware model of the full-connector re-enable edge that ends a
 * software replug: both lanes were disabled TOGETHER, held for @held
 * passes, and are now enabled TOGETHER. This is the replug-equivalent
 * electrical train (live 2026-08-23 appmana-001): the peer re-runs its
 * connector handshake, so BOTH lanes train -- including a secondary that
 * single-lane edges can never wake (lane1_full_train_only).
 *
 * Wedge rules (the rig's safety contract):
 *  - an edge while the resident firmware is mid-transition (icm_busy)
 *    races it: the 05d1e4c/2026-08-22 class;
 *  - an edge on a connector whose router is still enumerated is the
 *    2026-08-20 live-primary bounce and stays forbidden -- the replug
 *    must tear the software topology down FIRST.
 * A hold shorter than the firmware's busy window is a glitch edge, not a
 * replug: the disable edge set the firmware busy, and enabling inside
 * that window is the same race.
 */
static inline void cm_hw_connector_edge(struct cm_host *h, int p, int held)
{
	struct cm_port *port = &h->port[p];

	port->connector_edges++;
	if (h->resident_icm && port->icm_busy > 0) {
		h->icm_wedged = true;
		return;
	}
	if (port->xdomain) {
		h->icm_wedged = true;
		return;
	}
	if (held < CM_REPLUG_HOLD_PASSES)
		return;
	port->detection_rearmed = true;
	port->latched_off = false;
	if (!port->dead) {
		port->half_trained = false;
		port->link_up = true;
		if (port->lane1_sample && !port->lane1_dead)
			port->lane1_sample = CM_SAMPLE_UP;
	}
	if (h->resident_icm)
		port->icm_busy = CM_ICM_BUSY_PASSES;
}

/*
 * Lockstep with tb.c tb_port_connector_replug_start(): once the bounded
 * single-lane attempts on a CONNECTING secondary are exhausted, escalate
 * to a full-connector software replug. Keys ONLY on a secondary parked at
 * CONNECTING -- hardware provably present on lane 1 -- never on an absent
 * or UNPLUGGED secondary (a single-lane cable must be left alone). The
 * caller (cm_lane1_recover) has already enforced stability, cooldown,
 * spacing and hint-age discipline. Episodes are budgeted, and the budget
 * re-arms only after a long idle window, so permanently-degraded hardware
 * gets a bounded, slow-rate retry instead of a teardown loop.
 */
static inline void cm_connector_replug_start(struct cm_host *h, int p)
{
	struct cm_port *port = &h->port[p];

	if (port->lane1_sample != CM_SAMPLE_CONNECTING)
		return;
	if (port->replug_phase != CM_REPLUG_IDLE)
		return;
	if (port->replug_since_attempt < CM_RETRAIN_QUIET_PASSES)
		return;
	if (port->replug_attempts >= CM_REPLUG_MAX_ATTEMPTS) {
		if (port->replug_since_attempt < CM_REPLUG_IDLE_RESET_PASSES)
			return;
		port->replug_attempts = 0;
	}
	port->replug_attempts++;
	port->replug_since_attempt = 0;
	/* tb.c queues the synthesized unplug here (tb_queue_hotplug). */
	port->replug_phase = CM_REPLUG_TEARDOWN;
}

/*
 * Lockstep with tb.c tb_port_connector_replug_step(): the phase machine
 * the reconcile steps while an episode is in flight.
 *
 *  TEARDOWN: the queued synthesized unplug has landed, so the software
 *  topology below the connector is gone; disable BOTH lanes together.
 *  The disable is itself an edge the resident firmware consumes.
 *
 *  HELD: keep both lanes down for the full hold (longer than the
 *  firmware's busy window, and long enough that the peer treats the next
 *  enable as a fresh connector handshake -- the live 2026-08-23 recovery
 *  used 12 s where sub-ms glitch edges failed). A fresh firmware
 *  notification extends the hold: re-enabling into the firmware's busy
 *  window is the 2026-08-22 wedge recipe. Then enable both lanes
 *  together; the fresh train rejoins the normal reconcile flow (lane UP,
 *  nothing enumerated -> synthesized plug -> bonding at enumeration).
 */
static inline void cm_connector_replug_step(struct cm_host *h, int p)
{
	struct cm_port *port = &h->port[p];

	switch (port->replug_phase) {
	case CM_REPLUG_TEARDOWN:
		port->xdomain = false;
		port->cooldown = CM_RETRAIN_QUIET_PASSES;
		port->lanes_held = true;
		port->replug_held = 0;
		if (h->resident_icm)
			port->icm_busy = CM_ICM_BUSY_PASSES;
		port->replug_phase = CM_REPLUG_HELD;
		break;
	case CM_REPLUG_HELD:
		port->replug_held++;
		if (port->replug_held < CM_REPLUG_HOLD_PASSES)
			break;
		/* a fresh firmware notification: extend the hold */
		if (h->hint_age > 0 && h->hint_age <= CM_RETRAIN_QUIET_PASSES)
			break;
		port->lanes_held = false;
		cm_hw_connector_edge(h, p, port->replug_held);
		port->replug_phase = CM_REPLUG_IDLE;
		break;
	}
}

/*
 * Lockstep with tb.c tb_port_lane1_recover(): bounded recovery of a
 * degraded x1 link whose secondary lane is half-trained (retrain the
 * SECONDARY lane only), and late lane bonding once the secondary lane is
 * up (upstream runs bonding only at enumeration and never again --
 * tb_switch_set_link_width(TB_LINK_WIDTH_DUAL) is the retry). Same quiet
 * -window/budget discipline as cm_plug_retrain(). Exhausted single-lane
 * budgets escalate to the bounded full-connector software replug.
 */
static inline void cm_lane1_recover(struct cm_host *h, int p)
{
	struct cm_port *port = &h->port[p];

	if (!port->lane1_sample || port->bonded)
		return;
	port->lane1_since_attempt++;
	if (port->lane1_sample == port->lane1_last) {
		port->lane1_stable++;
	} else {
		port->lane1_last = port->lane1_sample;
		port->lane1_stable = 0;
	}
	if (port->lane1_sample != CM_SAMPLE_CONNECTING &&
	    port->lane1_sample != CM_SAMPLE_UP)
		return;
	if (port->lane1_stable < CM_RETRAIN_QUIET_PASSES)
		return;
	if (port->cooldown > 0)
		return;
	if (port->lane1_since_attempt < CM_RETRAIN_QUIET_PASSES)
		return;
	/* a fresh firmware notification: the ICM may be mid-transition */
	if (h->hint_age > 0 && h->hint_age <= CM_RETRAIN_QUIET_PASSES)
		return;
	if (port->lane1_attempts >= CM_RETRAIN_MAX_ATTEMPTS) {
		cm_connector_replug_start(h, p);
		return;
	}
	port->lane1_attempts++;
	port->lane1_since_attempt = 0;
	if (port->lane1_sample == CM_SAMPLE_CONNECTING) {
		cm_hw_lane1_edge(h, p);
	} else {
		/* tb_switch_set_link_width(sw, TB_LINK_WIDTH_DUAL) */
		port->bonded = true;
		port->lane1_attempts = 0;
	}
}

static inline void cm_reconcile(struct cm_host *h)
{
	int i;

	if (!h->hotplug_armed)
		return;

	if (h->hint_age > 0)
		h->hint_age++;

	for (i = 0; i < 2; i++) {
		struct cm_port *port = &h->port[i];
		int sample = cm_lane_sample(h, i);

		if (port->icm_busy > 0)
			port->icm_busy--;
		if (port->cooldown > 0)
			port->cooldown--;
		port->since_attempt++;
		port->replug_since_attempt++;
		if (sample == port->last_sample) {
			port->stable++;
		} else {
			port->last_sample = sample;
			port->stable = 0;
		}

		if (port->replug_phase != CM_REPLUG_IDLE) {
			cm_connector_replug_step(h, i);
			continue;
		}

		if (port->xdomain && sample == CM_SAMPLE_UNPLUGGED &&
		    !cm_peer_probe(h, i)) {
			port->xdomain = false;	/* synthesized unplug */
			port->cooldown = CM_RETRAIN_QUIET_PASSES;
			/* tb.c: if (gone) tb_port_kick_detection(port) */
			cm_kick_detection(h, i);
		} else if (!port->xdomain && sample == CM_SAMPLE_UP) {
			/*
			 * The synthesized plug only ENUMERATES if the scan it
			 * triggers can reach the router. A lane that samples UP
			 * behind a mute router yields nothing, and the reconcile
			 * must keep retrying at its bounded rate rather than
			 * latching the port unenumerated forever.
			 */
			if (!cm_peer_probe(h, i)) {
				if (port->since_attempt < CM_RETRAIN_QUIET_PASSES)
					continue;
				port->since_attempt = 0;
				port->plug_synth_failed++;
				continue;
			}
			port->xdomain = true;	/* synthesized plug */
			port->attempts = 0;	/* episode over: it trained */
			/*
			 * tb_scan_port(): lane bonding is attempted at
			 * enumeration; with the secondary lane up it
			 * succeeds and the episodes are over.
			 */
			if (port->lane1_sample == CM_SAMPLE_UP) {
				port->bonded = true;
				port->lane1_attempts = 0;
				port->replug_attempts = 0;
			}
		} else if (port->xdomain && sample == CM_SAMPLE_UP) {
			cm_lane1_recover(h, i);
		} else if (!port->xdomain &&
			   (sample == CM_SAMPLE_CONNECTING ||
			    sample == CM_SAMPLE_UNPLUGGED)) {
			cm_plug_retrain(h, i, sample);
		}
	}
}

/*
 * ---- Stale cached identity loop (xdomain.c PROPERTIES vs UUID) ----
 *
 * Live evidence (2026-07-09, appmana-008 port 1): the XDomain was created with
 * a CORRUPT remote UUID (66518780-00e3-212c-ffff-ffffffffffff -- the tail is
 * all-ones, a half-trained-link read). XDP property requests carry dst_uuid
 * and the healthy peer ignores a mismatch, so every read fails. The PROPERTIES
 * state re-queued itself for 87 minutes (one "failed read XDomain properties"
 * per ~22 s budget cycle) and never recovered; recovery came only from an
 * external link edge at 15:33. The same loop absorbs ANY stale identity: a
 * peer that reboots into a different UUID while the survivor's cached identity
 * is wrong keeps failing reads forever.
 *
 * Upstream already has the right primitive: tb_xdomain_get_uuid() detects a
 * changed remote UUID and marks the XDomain is_unplugged for the connection
 * manager to replace. The bug is that the PROPERTIES retry loop NEVER goes
 * back through UUID verification, so that primitive is unreachable.
 *
 * ident_tick() is kept in LOCKSTEP with tb_xdomain_state_work(): while the
 * driver re-queues PROPERTIES blindly, the model must too (recovery test goes
 * red). The fix routes PROPERTIES exhaustion back through the UUID state; a
 * UUID mismatch marks the xd unplugged and the CM reconciliation replaces it.
 */
#define IDENT_STATE_PROPERTIES	0
#define IDENT_STATE_UUID	1

/*
 * A UUID read back off a link with no powered peer behind it: every config
 * dword reads all-ones, so the identity's low half is 0xffff'ffff'ffff'ffff.
 * The live samples all carry this exact tail (385a8780-0004-402c-ffff-
 * ffffffffffff, 66518780-00e3-212c-ffff-ffffffffffff). Lockstep with
 * tb_xdomain_uuid_is_placeholder() in thunderbolt_negotiation.h; the model
 * keeps identities in one u32 so the sentinel is the all-ones word.
 */
#define IDENT_UUID_PLACEHOLDER	0xffffffffu

struct ident_peer {
	u32 true_uuid;		/* identity the peer answers with */
	bool answers;		/* peer booted/responsive */
};

struct ident_xd {
	u32 cached_uuid;	/* xd->remote_uuid */
	int state;		/* IDENT_STATE_* */
	bool uuid_verified;	/* active route-local UUID response observed */
	bool cached_properties; /* xd->remote_properties: enumerated before */
	bool enumerated;	/* properties read; services probed */
	bool unplugged;		/* xd->is_unplugged: waiting for CM replace */
};

/* XDP properties request: dst_uuid must match or the peer ignores it. */
static inline bool ident_properties_read(const struct ident_xd *xd,
					 const struct ident_peer *peer)
{
	return peer->answers && xd->cached_uuid == peer->true_uuid;
}

/*
 * One PROPERTIES/UUID budget cycle, lockstep with tb_xdomain_state_work():
 * a failed PROPERTIES budget re-verifies the cached identity via the UUID
 * state; tb_xdomain_get_uuid() marks a mismatch is_unplugged (-ENODEV).
 */
static inline void ident_tick(struct ident_xd *xd, const struct ident_peer *peer)
{
	if (xd->enumerated || xd->unplugged)
		return;

	switch (xd->state) {
	case IDENT_STATE_PROPERTIES:
		if (ident_properties_read(xd, peer)) {
			/* A response addressed back to us proves the ICM claim. */
			xd->uuid_verified = true;
			xd->enumerated = true;
		} else {
			xd->state = IDENT_STATE_UUID;	/* re-verify identity */
		}
		break;
	case IDENT_STATE_UUID:
		if (!peer->answers) {
			/*
			 * One full UUID retry budget is one model tick.
			 * Previously-enumerated objects are stale topology:
			 * replace them so a fresh handshake can discover a
			 * rebooted peer. A first-boot object has no cached
			 * properties and preserves the unbounded co-reset
			 * retry.
			 */
			if (xd->cached_properties)
				xd->unplugged = true;
			break;
		}
		if (xd->cached_uuid == peer->true_uuid) {
			xd->uuid_verified = true;
			xd->state = IDENT_STATE_PROPERTIES;
		} else if (!xd->uuid_verified ||
			   xd->cached_uuid == IDENT_UUID_PLACEHOLDER) {
			/*
			 * Not "another domain connected" -- this domain finally
			 * answering. The CM can hand us an XDomain for a link
			 * whose peer is powered off, and the identity it carries
			 * is then the all-ones-tail read. Adopt the real one
			 * (tb_xdomain_uuid_is_placeholder in
			 * tb_xdomain_get_uuid).
			 */
			xd->cached_uuid = peer->true_uuid;
			xd->uuid_verified = true;
			xd->state = IDENT_STATE_PROPERTIES;
		} else {
			xd->unplugged = true;	/* tb_xdomain_get_uuid mismatch */
		}
		break;
	}
}

/*
 * The CM replacing an is_unplugged XDomain (synthesized unplug + rescan of the
 * still-up port): the fresh XDomain reads the peer's real UUID.
 */
static inline void ident_cm_replace(struct ident_xd *xd,
				    const struct ident_peer *peer)
{
	if (!xd->unplugged)
		return;
	xd->cached_uuid = peer->true_uuid;
	xd->state = IDENT_STATE_PROPERTIES;
	xd->uuid_verified = false;
	xd->cached_properties = false;
	xd->unplugged = false;
}

static inline bool ident_run(struct ident_xd *xd, const struct ident_peer *peer,
			     int cycles, bool cm_reconciles)
{
	while (cycles-- > 0) {
		ident_tick(xd, peer);
		if (cm_reconciles)
			ident_cm_replace(xd, peer);
		if (xd->enumerated)
			return true;
	}
	return xd->enumerated;
}

/*
 * ---- Properties-changed announcement to an absent peer ----
 *
 * Live 2026-08-24: appmana-008 (Maple Ridge AM5 chain node) booted while its
 * chain neighbour appmana-019 was POWERED OFF. 008's port facing 019 still
 * enumerated an XDomain (0-3) and logged, repeatedly,
 *
 *   thunderbolt 0-3: failed to send properties changed notification
 *   thunderbolt 0-3: failed read XDomain properties from
 *                    385a8780-0004-402c-ffff-ffffffffffff
 *
 * tb_xdomain_properties_changed() re-queues itself only while
 * properties_changed_retries lasts (XDOMAIN_RETRIES attempts at a fixed
 * XDOMAIN_DEFAULT_TIMEOUT spacing). Against an unpowered peer the whole budget
 * burns in ~10 s, after which the work is NEVER queued again and the host stops
 * announcing its property directory for the life of the XDomain. When 019
 * finally powered on, nothing on 008 re-announced, so 019's ICM never saw an
 * inbound XDP request, 019 sent tbframe HELLOs forever and 008 never answered.
 * A leaf-only reload of the frame drivers cannot fix this: the announce lives
 * below them, in the core.
 *
 * The dev_err was unconditional too -- emitted on the retrying path as well as
 * the giving-up path -- which is why one absent peer produced a burst of
 * identical lines rather than a single give-up notice.
 *
 * A chain neighbour powering on later is normal operation, so the announce must
 * never stop; but an absent peer must not cost a control packet per second nor
 * a log line per attempt forever. Modelled against a virtual clock so a test can
 * assert persistence AND cost. Kept in LOCKSTEP with
 * tb_xdomain_properties_changed() and calling the SAME production policy
 * functions it does (tb_xdomain_announce_delay_ms(),
 * tb_xdomain_announce_should_warn()), so the two cannot drift on the numbers.
 */
struct ctl_rx_packet_model {
	unsigned int eof;
	unsigned int size;
	u8 code;
	u8 flags;
	u8 packet_id;
	u8 total_packets;
};

enum ctl_submit_model_state {
	CTL_SUBMIT_MODEL_WAITING,
	CTL_SUBMIT_MODEL_ACCEPTED,
	CTL_SUBMIT_MODEL_FAILED,
	CTL_SUBMIT_MODEL_TIMED_OUT,
};

enum ctl_peer_model_state {
	CTL_PEER_MODEL_WAITING,
	CTL_PEER_MODEL_MATCHED,
	CTL_PEER_MODEL_CANCELED,
	CTL_PEER_MODEL_TIMED_OUT,
};

enum ctl_transaction_model_event {
	CTL_TRANSACTION_MODEL_LOCAL_ACCEPTED,
	CTL_TRANSACTION_MODEL_LOCAL_FAILED,
	CTL_TRANSACTION_MODEL_PEER_MATCHED,
	CTL_TRANSACTION_MODEL_PEER_TIMED_OUT,
};

enum ctl_transaction_model_action {
	CTL_TRANSACTION_MODEL_NONE,
	CTL_TRANSACTION_MODEL_COMPLETE,
	CTL_TRANSACTION_MODEL_FAIL,
};

struct ctl_transaction_model {
	enum ctl_submit_model_state submit;
	enum ctl_peer_model_state peer;
};

struct ctl_xdomain_packet_model {
	bool owns_local_completion;
	bool waits_for_peer;
};

static inline struct ctl_xdomain_packet_model
ctl_model_xdomain_packet(bool expects_peer_response)
{
	return (struct ctl_xdomain_packet_model) {
		.owns_local_completion = true,
		.waits_for_peer = expects_peer_response,
	};
}

static inline bool ctl_model_service_response_deferred(void)
{
	/* Service callbacks can run in the control RX dispatch worker. */
	return true;
}

static inline int
ctl_model_local_only_result(enum ctl_submit_model_state state,
			    int current_result)
{
	if (current_result)
		return current_result;
	if (state == CTL_SUBMIT_MODEL_FAILED)
		return -EIO;
	if (state == CTL_SUBMIT_MODEL_TIMED_OUT)
		return -ETIMEDOUT;
	return 0;
}

static inline bool
ctl_model_response_should_retry(bool local_failed, bool local_timed_out,
				int result, unsigned int attempt)
{
	return local_failed && !local_timed_out && result == -EIO && attempt < 9;
}

enum ctl_match_owner_model {
	CTL_MATCH_MODEL_GENERIC,
	CTL_MATCH_MODEL_PEER,
	CTL_MATCH_MODEL_NONE,
};

static inline enum ctl_match_owner_model
ctl_model_request_match_owner(bool local_sender, bool peer_waiter)
{
	if (peer_waiter)
		return CTL_MATCH_MODEL_PEER;
	if (local_sender)
		return CTL_MATCH_MODEL_NONE;
	return CTL_MATCH_MODEL_GENERIC;
}

static inline enum ctl_transaction_model_action
ctl_model_transaction_step(struct ctl_transaction_model *state,
			   enum ctl_transaction_model_event event)
{
	switch (event) {
	case CTL_TRANSACTION_MODEL_LOCAL_ACCEPTED:
		if (state->submit == CTL_SUBMIT_MODEL_WAITING)
			state->submit = CTL_SUBMIT_MODEL_ACCEPTED;
		return CTL_TRANSACTION_MODEL_NONE;
	case CTL_TRANSACTION_MODEL_LOCAL_FAILED:
		if (state->submit != CTL_SUBMIT_MODEL_WAITING)
			return CTL_TRANSACTION_MODEL_NONE;
		state->submit = CTL_SUBMIT_MODEL_FAILED;
		/* The route-only local status cannot own the sequenced peer wait. */
		return CTL_TRANSACTION_MODEL_NONE;
	case CTL_TRANSACTION_MODEL_PEER_MATCHED:
		if (state->peer != CTL_PEER_MODEL_WAITING)
			return CTL_TRANSACTION_MODEL_NONE;
		state->peer = CTL_PEER_MODEL_MATCHED;
		return CTL_TRANSACTION_MODEL_COMPLETE;
	case CTL_TRANSACTION_MODEL_PEER_TIMED_OUT:
		if (state->peer != CTL_PEER_MODEL_WAITING)
			return CTL_TRANSACTION_MODEL_NONE;
		state->peer = CTL_PEER_MODEL_TIMED_OUT;
		if (state->submit == CTL_SUBMIT_MODEL_WAITING)
			state->submit = CTL_SUBMIT_MODEL_TIMED_OUT;
		return CTL_TRANSACTION_MODEL_FAIL;
	default:
		return CTL_TRANSACTION_MODEL_NONE;
	}
}

/* Independent wire oracle: numeric values and length come from the capture. */
static inline bool
ctl_model_is_xdomain_tx_status(const struct ctl_rx_packet_model *packet)
{
	return packet->eof == 12 && packet->size == 28 &&
	       packet->code == 8 && packet->packet_id == 0 &&
	       packet->total_packets == 1;
}

struct announce_state {
	unsigned int now_ms;	/* virtual clock */
	unsigned int attempts;	/* wire attempts made */
	unsigned int errs;	/* dev_err lines emitted */
	unsigned int failures;	/* xd->properties_changed_retries */
	bool armed;		/* properties_changed_work is queued */
	bool delivered;		/* the peer acknowledged the notification */
	bool stopped;		/* TB_XDOMAIN_ANNOUNCE_STOPPED published */
	bool peer_previously_proven; /* a response established this peer */
	bool inbound_peer_control; /* peer still transmits on this route */
	bool inbound_during_outbound_wait;
	bool recovery_requested; /* controller recovery was escalated */
	enum tb_xdomain_control_state control_state;
};

/* Independent policy oracle for a peer that remains absent after fast probe. */
static inline unsigned int
identity_model_retry_delay_ms(unsigned int failures)
{
	const unsigned int max_shift = 6;

	return 1000u << min(failures, max_shift);
}

/* tb_xdomain_queue_properties_changed(): arm (or re-arm) the announcement. */
static inline void announce_arm(struct announce_state *a)
{
	a->armed = true;
	a->stopped = false;
	a->failures = 0;
}

/* __stop_handshake(): publish the marker, then cancel. */
static inline void announce_stop(struct announce_state *a)
{
	a->stopped = true;
	a->armed = false;
}

/* A route-correlated inbound request proves that the known peer is present. */
static inline void announce_note_inbound_peer_control(struct announce_state *a)
{
	a->inbound_peer_control = true;
	a->control_state = tb_xdomain_control_next(
		a->control_state, TB_XDOMAIN_CONTROL_INBOUND_PROGRESS);
}

static inline void
announce_note_concurrent_inbound_peer_control(struct announce_state *a)
{
	announce_note_inbound_peer_control(a);
	a->inbound_during_outbound_wait = true;
}

/* One scheduled run of properties_changed_work. */
static inline void announce_tick(struct announce_state *a, bool peer_present)
{
	if (!a->armed)
		return;

	a->now_ms += tb_xdomain_announce_delay_ms(a->failures);
	a->attempts++;

	if (peer_present) {
		a->delivered = true;
		a->failures = 0;
		a->control_state = tb_xdomain_control_next(
			a->control_state, TB_XDOMAIN_CONTROL_OUTBOUND_SUCCEEDED);
		a->armed = false;	/* nothing left to announce */
		a->inbound_during_outbound_wait = false;
		return;
	}

	/*
	 * The stop marker is re-read AFTER the bounded wire wait, so a stop that
	 * lands while this attempt is in flight wins and the work does not
	 * re-arm past its cancel_delayed_work_sync().
	 */
	if (a->stopped) {
		a->armed = false;
		return;
	}

	if (tb_xdomain_announce_should_warn(a->failures))
		a->errs++;
	if (a->failures <= TB_XDOMAIN_ANNOUNCE_MAX_SHIFT)
		a->failures++;
	if (a->peer_previously_proven && a->inbound_peer_control &&
	    a->inbound_during_outbound_wait &&
	    a->failures > TB_XDOMAIN_ANNOUNCE_MAX_SHIFT) {
		a->control_state = tb_xdomain_control_next(
			a->control_state,
			TB_XDOMAIN_CONTROL_OUTBOUND_SATURATED);
		if (a->control_state ==
		    TB_XDOMAIN_CONTROL_RECOVERY_REQUIRED) {
			a->recovery_requested = true;
			a->control_state = tb_xdomain_control_next(
				a->control_state,
				TB_XDOMAIN_CONTROL_RECOVERY_DISPATCHED);
		}
	}
	a->inbound_during_outbound_wait = false;
	a->armed = true;		/* the announce never gives up */
}

/* An attempt whose bounded wire wait overlaps a __stop_handshake(). */
static inline void announce_tick_stopped_midflight(struct announce_state *a)
{
	if (!a->armed)
		return;

	a->now_ms += tb_xdomain_announce_delay_ms(a->failures);
	a->attempts++;
	announce_stop(a);		/* the marker lands inside the wait */
	a->armed = false;
}

/* Run properties_changed_work for @wall_ms of virtual time. */
static inline void announce_run(struct announce_state *a, bool peer_present,
				unsigned int wall_ms)
{
	unsigned int end = a->now_ms + wall_ms;

	while (a->armed && a->now_ms < end) {
		announce_tick(a, peer_present);
		if (a->delivered)
			return;
	}
}

/*
 * ---- Two-port Maple Ridge lane-bonding scheduler ----
 *
 * A physical TB4 cable has TWO bidirectional lanes. Each Maple Ridge connector
 * owns one such two-lane link to one peer; connectors are never bonded to one
 * another. Linux represents the two lanes of one connector as a lane adapter
 * and its dual_link_port.
 *
 * Live appmana-023/025 evidence (2026-07-31):
 *
 *   023 port 0 -> older/unresponsive 022
 *   023 port 1 -> 025 port 0
 *   025 port 0 -> 023 port 1
 *   025 port 1 -> older 009
 *
 * tb_xdomain_lane_bonding_enable() waits synchronously for width DUAL for up
 * to XDOMAIN_BONDING_TIMEOUT (10 seconds). XDomain state_work for both ports
 * shares the controller's ordered workqueue. Consequently 023 first blocks on
 * 022 while 025 targets 023; one timeout later 023 targets 025 while 025 blocks
 * on 009. The two ends are individually capable but their target-DUAL windows
 * never overlap. This is exactly the cold-boot trace: 025 returned -ETIMEDOUT,
 * cleaned up, and only later did 023 return -ENOTCONN.
 *
 * The model deliberately includes two ports and distinct peers. A model with
 * one host/one cable cannot reproduce this scheduler bug and would falsely
 * bless the synchronous implementation.
 */
#define BOND_MODEL_PORTS 2
#define BOND_MODEL_WAIT_TICKS 10

struct bond_model_port {
	int peer_port;		/* matching port on the other model host, -1 = legacy */
	int window_start;
	int window_end;
	bool capable;
	bool bonded;
	bool services_published;
	bool services_republished;
};

struct bond_model_host {
	struct bond_model_port port[BOND_MODEL_PORTS];
};

struct bond_model_link {
	struct bond_model_host a;
	struct bond_model_host b;
	bool destabilized;
};

static inline void bond_model_appmana_023_025(struct bond_model_link *L)
{
	memset(L, 0, sizeof(*L));

	/* 023 visits the unresponsive 022-facing port before its 025 port. */
	L->a.port[0].peer_port = -1;
	L->a.port[0].capable = false;
	L->a.port[1].peer_port = 0;
	L->a.port[1].capable = true;

	/* 025 visits its 023-facing port before the older 009-facing port. */
	L->b.port[0].peer_port = 1;
	L->b.port[0].capable = true;
	L->b.port[1].peer_port = -1;
	L->b.port[1].capable = false;
}

static inline bool bond_model_windows_overlap(const struct bond_model_port *a,
					       const struct bond_model_port *b)
{
	return a->window_start < b->window_end &&
	       b->window_start < a->window_end;
}

static inline void bond_model_schedule_host(struct bond_model_host *h,
					    bool controller_blocking)
{
	int i;

	for (i = 0; i < BOND_MODEL_PORTS; i++) {
		h->port[i].window_start = controller_blocking ?
			i * BOND_MODEL_WAIT_TICKS : 0;
		h->port[i].window_end = h->port[i].window_start +
			BOND_MODEL_WAIT_TICKS;
	}
}

static inline void bond_model_run(struct bond_model_link *L,
				  bool controller_blocking)
{
	int i;

	bond_model_schedule_host(&L->a, controller_blocking);
	bond_model_schedule_host(&L->b, controller_blocking);

	for (i = 0; i < BOND_MODEL_PORTS; i++) {
		struct bond_model_port *a = &L->a.port[i];
		struct bond_model_port *b;

		if (!a->capable || a->peer_port < 0)
			continue;
		b = &L->b.port[a->peer_port];
		if (!b->capable || b->peer_port != i)
			continue;
		if (!bond_model_windows_overlap(a, b))
			continue;
		if (a->services_published || b->services_published) {
			L->destabilized = true;
			continue;
		}
		a->bonded = true;
		b->bonded = true;
	}
}

static inline bool bond_model_pair_bonded(const struct bond_model_link *L)
{
	return L->a.port[1].bonded && L->b.port[0].bonded;
}

/*
 * ---- XDomain response demultiplexing ----
 *
 * A control response belongs to one request only when its route, protocol,
 * sequence and response operation all agree. The receive size is expressed in
 * bytes throughout: fixed responses fill their request buffer, while a
 * properties response may be shorter than its capacity but must include its
 * complete fixed header. This model is the protocol oracle for the production
 * matcher tests; it does not call the production matcher.
 */
struct xdp_match_model_request {
	u64 route;
	enum tb_xdp_type type;
	u8 sequence;
	size_t response_capacity;
	u32 protocol;
};

struct xdp_match_model_response {
	u64 route;
	enum tb_xdp_type type;
	u8 sequence;
	size_t size;
	size_t declared_size;
	u32 protocol;
};

static inline enum tb_xdp_type
xdp_match_model_response_type(enum tb_xdp_type request_type)
{
	switch (request_type) {
	case UUID_REQUEST_OLD:
	case UUID_REQUEST:
		return UUID_RESPONSE;
	case PROPERTIES_REQUEST:
		return PROPERTIES_RESPONSE;
	case PROPERTIES_CHANGED_REQUEST:
		return PROPERTIES_CHANGED_RESPONSE;
	case LINK_STATE_STATUS_REQUEST:
		return LINK_STATE_STATUS_RESPONSE;
	case LINK_STATE_CHANGE_REQUEST:
		return LINK_STATE_CHANGE_RESPONSE;
	default:
		return 0;
	}
}

static inline size_t
xdp_match_model_min_size(enum tb_xdp_type response_type)
{
	switch (response_type) {
	case UUID_RESPONSE:
		return sizeof(struct tb_xdp_uuid_response);
	case PROPERTIES_RESPONSE:
		return sizeof(struct tb_xdp_properties_response);
	case PROPERTIES_CHANGED_RESPONSE:
		return sizeof(struct tb_xdp_properties_changed_response);
	case LINK_STATE_STATUS_RESPONSE:
		return sizeof(struct tb_xdp_link_state_status_response);
	case LINK_STATE_CHANGE_RESPONSE:
		return sizeof(struct tb_xdp_link_state_change_response);
	case ERROR_RESPONSE:
		return sizeof(struct tb_xdp_error_response);
	default:
		return SIZE_MAX;
	}
}

static inline bool
xdp_match_model_matches(const struct xdp_match_model_request *request,
			const struct xdp_match_model_response *response)
{
	enum tb_xdp_type expected = xdp_match_model_response_type(request->type);
	size_t minimum = xdp_match_model_min_size(response->type);

	if (!expected || minimum == SIZE_MAX)
		return false;
	if (response->route != request->route ||
	    response->protocol != request->protocol ||
	    response->sequence != request->sequence)
		return false;
	if (response->type != expected && response->type != ERROR_RESPONSE)
		return false;
	if (response->declared_size != response->size)
		return false;
	return response->size >= minimum &&
	       response->size <= request->response_capacity;
}

/*
 * ---- Bonding re-arm (from ENUMERATED) ----
 *
 * Bounded retries from ENUMERATED with a PASSIVE high side: an
 * enumerated, capable, unbonded peer accepts a link-state-change request
 * at any time (it re-enters BONDING_UUID_HIGH on demand), so the low
 * side's attempt wakes it and the two ends' windows no longer need to
 * overlap - which is what made boot skew the terminal x1 fixed point.
 * A success on a link whose services are already published re-announces
 * the property dirs (generation bump; never unregister/re-register live
 * dirs) so the service sessions re-establish their DMA paths with bonded
 * credits; that is what keeps a late bond from destabilizing the link the
 * way mid-session bonding did (c1777f9). Failure exhausts the budget
 * quietly and never touches lane adapters (b5f07da).
 */
static inline void bond_model_rearm(struct bond_model_link *L, int attempts)
{
	int i, try;

	for (try = 0; try < attempts; try++) {
		for (i = 0; i < BOND_MODEL_PORTS; i++) {
			struct bond_model_port *a = &L->a.port[i];
			struct bond_model_port *b;

			if (!a->capable || a->peer_port < 0)
				continue;
			b = &L->b.port[a->peer_port];
			if (!b->capable || b->peer_port != i)
				continue;
			if (a->bonded && b->bonded)
				continue;
			/* Passive high side: no window overlap required. */
			a->bonded = true;
			b->bonded = true;
			if (a->services_published)
				a->services_republished = true;
			if (b->services_published)
				b->services_republished = true;
		}
	}
}

#endif /* _TB_NEGOTIATION_MODEL_H */
