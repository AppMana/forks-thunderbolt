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

#endif /* _TB_NEGOTIATION_MODEL_H */
