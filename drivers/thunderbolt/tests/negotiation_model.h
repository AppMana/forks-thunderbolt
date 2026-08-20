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
};

struct cm_host {
	struct cm_port port[2];
	bool hotplug_armed;
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
	for (i = 0; i < 2; i++)
		cm_scan_port(h, i);
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
static inline void cm_link_up_lost(struct cm_host *h, int p)
{
	h->port[p].link_up = true;	/* plug event lost: no scan happens */
}

static inline void cm_link_down_lost(struct cm_host *h, int p)
{
	h->port[p].link_up = false;	/* unplug event lost: xdomain stays */
}

/*
 * What tb_reconcile_work() actually observes: one lane-state register read.
 * A dead peer reads UNPLUGGED -- but so does a live link whose state was
 * perturbed by a coexisting master or a CLx transition (state_perturbed).
 */
static inline bool cm_lane_sample_unplugged(const struct cm_host *h, int p)
{
	return !h->port[p].link_up || h->port[p].state_perturbed;
}

/*
 * Bounded config-space probe of the enumerated child/peer router. A dead
 * peer's router is unpowered and cannot answer; a live one answers no
 * matter what its lane-state register momentarily reads.
 */
static inline bool cm_peer_probe(const struct cm_host *h, int p)
{
	return h->port[p].link_up;
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
static inline void cm_reconcile(struct cm_host *h)
{
	int i;

	if (!h->hotplug_armed)
		return;

	for (i = 0; i < 2; i++) {
		if (h->port[i].xdomain && cm_lane_sample_unplugged(h, i) &&
		    !cm_peer_probe(h, i))
			h->port[i].xdomain = false;	/* synthesized unplug */
		else if (!h->port[i].xdomain && h->port[i].link_up)
			h->port[i].xdomain = true;	/* synthesized plug */
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

struct ident_peer {
	u32 true_uuid;		/* identity the peer answers with */
	bool answers;		/* peer booted/responsive */
};

struct ident_xd {
	u32 cached_uuid;	/* xd->remote_uuid */
	int state;		/* IDENT_STATE_* */
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
		if (ident_properties_read(xd, peer))
			xd->enumerated = true;
		else
			xd->state = IDENT_STATE_UUID;	/* re-verify identity */
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
		if (xd->cached_uuid != peer->true_uuid)
			xd->unplugged = true;	/* tb_xdomain_get_uuid mismatch */
		else
			xd->state = IDENT_STATE_PROPERTIES;
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
