/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Deterministic two-host model of Thunderbolt XDomain connection negotiation,
 * with a MOCKED ICM firmware reproducing the decompiled behaviour, used to
 * model the soft-reconnect hang and validate the fix.
 *
 * Shared verbatim by the KUnit suite (test.c) and the userspace mirror. Pure C,
 * depends only on the shared negotiation header. Keep copies in lockstep.
 *
 * Mocked firmware (Titan Ridge JHL7540 / Maple Ridge JHL8540 disassembly,
 * ../../icm-firmware-re/): ICM_EVENT_XDOMAIN_CONNECTED fires to the host ONLY on
 * the rising edge of physical link-present, latched one-shot; no
 * property-generation comparison, no re-notify on a property-only change. So a
 * software reload toggles no link edge and the firmware stays silent.
 *
 * Two software layers, both per the common header:
 *  1. Property exchange with a MONOTONIC generation, gated by
 *     tb_xdomain_generation_stale() (a re-read is accepted only if newer; a
 *     cached gen of 0 forces accept).
 *  2. A login/HELLO handshake with a FINITE retry budget. A host can RESPOND
 *     to a HELLO only after it has settled (settle == 0) following a reload.
 *
 * The hang: a coordinated reload bumps both generations and resets both
 * handshakes, but both spend their finite budgets while still settling, so
 * every HELLO is lost and neither side re-arms. The firmware never helps (no
 * link edge). The fix (fix_rearm): re-arm the handshake whenever the peer's
 * generation advances (the soft-reconnect signal, observed via the re-read) and
 * keep retrying until established.
 */
#ifndef _TB_NEGOTIATION_MODEL_H
#define _TB_NEGOTIATION_MODEL_H

#include "../thunderbolt_negotiation.h"

/*
 * Defaults; the real values are per-host (set at boot) so tests can sweep them
 * and prove the hang is a genuine budget-vs-settle fragility, not a hardcoded
 * failure.
 */
#define HS_RETRY_BUDGET 3	/* finite login/HELLO retries, like the drivers */
#define SETTLE_STEPS    5	/* steps after reload before a host can respond */

struct mock_icm {
	bool link_present;
	bool xdomain_announced;	/* one-shot latch, cleared only on link-down */
};

/* Fires true once on the rising link edge; never on a property-only change. */
static inline bool mock_icm_connected_event(struct mock_icm *fw)
{
	if (fw->link_present && !fw->xdomain_announced) {
		fw->xdomain_announced = true;
		return true;
	}
	return false;
}

struct model_host {
	/* advertised */
	unsigned int local_gen;		/* monotonic property generation */
	bool service_registered;
	bool driver_up;
	int settle;			/* >0: reloading, cannot respond yet */

	/* core view of the peer (persists across a driver reload) */
	bool have_remote;
	unsigned int cached_remote_gen;
	bool remote_service_present;

	/* driver handshake (shared struct) */
	struct tb_xdomain_handshake hs;
	unsigned int hs_gen;		/* peer gen we last (re-)armed against */
	bool fix_rearm;			/* THE FIX: re-arm + retry until established */

	/* per-host timing (so tests can sweep them) */
	int settle_steps;
	int retry_budget;

	struct mock_icm fw;
};

struct model_link {
	struct model_host a, b;
};

/* core: re-read peer properties through the generation gate (force bypasses) */
static inline void model_core_reread(struct model_host *self,
				     const struct model_host *peer, bool force)
{
	unsigned int cached = force ? 0 : self->cached_remote_gen;

	if (tb_xdomain_generation_stale(self->have_remote, peer->local_gen, cached))
		return;
	self->have_remote = true;
	self->cached_remote_gen = peer->local_gen;
	self->remote_service_present = peer->service_registered;
}

/* firmware-driven enumeration: only on a physical link edge */
static inline void model_poll_firmware(struct model_host *self,
				       const struct model_host *peer)
{
	if (mock_icm_connected_event(&self->fw))
		model_core_reread(self, peer, /*force=*/true);
}

/* self (re)transmits its HELLO; re-arms on a peer-generation change if fixed */
static inline void model_send_hello(struct model_host *self, struct model_host *peer)
{
	/*
	 * THE FIX: a soft reconnect shows up as the peer's generation advancing.
	 * Re-arm the handshake against the new generation -- BEFORE the
	 * "established" short-circuit, because our established state is stale
	 * once the peer has restarted -- so a latched handshake retries instead
	 * of believing a dead connection is still up.
	 */
	if (self->fix_rearm && self->have_remote &&
	    self->cached_remote_gen != self->hs_gen) {
		tb_xdomain_handshake_reset(&self->hs);
		self->hs_gen = self->cached_remote_gen;
	}

	/*
	 * Mutual handshake: the link is up only once BOTH sides have seen each
	 * other's HELLO. A reloaded host cannot confirm (set its peer_seen)
	 * until it has settled, so the stable peer must keep retrying past the
	 * reloaded peer's settle window -- which the finite budget cannot.
	 */
	bool link_up = self->hs.peer_seen && peer->hs.peer_seen;

	if (!self->driver_up || !self->remote_service_present || link_up)
		return;

	/* without the fix the finite budget is spent one-shot and we give up */
	if (!self->fix_rearm && (int)self->hs.attempts >= self->retry_budget)
		return;

	self->hs.attempts++;
	self->hs.request_sent = true;
	/* our HELLO lands only if the peer has settled enough to respond */
	if (peer->driver_up && peer->settle == 0 && peer->remote_service_present)
		peer->hs.peer_seen = true;
}

static inline void model_step(struct model_link *L)
{
	if (L->a.settle > 0)
		L->a.settle--;
	if (L->b.settle > 0)
		L->b.settle--;

	model_poll_firmware(&L->a, &L->b);
	model_poll_firmware(&L->b, &L->a);

	/* a soft reconnect re-reads the peer (the notification/poll); the fix's
	 * re-arm keys off the generation this surfaces */
	model_core_reread(&L->a, &L->b, /*force=*/L->a.fix_rearm);
	model_core_reread(&L->b, &L->a, /*force=*/L->b.fix_rearm);

	model_send_hello(&L->a, &L->b);
	model_send_hello(&L->b, &L->a);

	if (L->a.hs.peer_seen && L->b.hs.peer_seen) {
		L->a.hs.established = true;
		L->b.hs.established = true;
	}
}

static inline void model_run(struct model_link *L, int steps)
{
	while (steps-- > 0)
		model_step(L);
}

static inline bool model_established(const struct model_link *L)
{
	return L->a.hs.established && L->b.hs.established;
}

/* ---- operations ---- */
static inline void model_host_boot(struct model_host *h, bool fix,
				   int settle_steps, int retry_budget)
{
	h->driver_up = true;
	h->service_registered = true;
	h->local_gen++;
	h->settle = 0;
	h->fix_rearm = fix;
	h->settle_steps = settle_steps;
	h->retry_budget = retry_budget;
	h->fw.link_present = true;
	tb_xdomain_handshake_reset(&h->hs);
}

/* a soft driver reload: tear down + rebuild, NO link edge */
static inline void model_host_reload(struct model_host *h)
{
	h->driver_up = false;
	h->service_registered = false;
	h->local_gen++;			/* unregister bumps gen */

	h->driver_up = true;
	h->service_registered = true;
	h->local_gen++;			/* re-register bumps gen */
	h->settle = h->settle_steps;
	tb_xdomain_handshake_reset(&h->hs);
}

#endif /* _TB_NEGOTIATION_MODEL_H */
