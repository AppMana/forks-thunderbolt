/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Thunderbolt XDomain connection-negotiation primitives, shared by the
 * thunderbolt core, thunderbolt_net and thunderbolt_ibverbs.
 *
 * All three negotiate a host-to-host connection over the same XDomain control
 * channel (ring 0) and suffer the same class of bug: after a SOFT reconnect
 * (a service-driver reload, or a peer re-registering its property directory)
 * WITHOUT a physical link edge, the connection fails to re-establish. Two
 * mechanisms combine:
 *
 *   1. Generation gate. The core's remote property block carries a MONOTONIC
 *      generation; a re-read is accepted only if strictly newer. The ICM
 *      firmware only re-announces XDomain on a physical link edge (verified by
 *      disassembly of Titan Ridge JHL7540 and Maple Ridge JHL8540 ICM
 *      firmware: ICM_EVENT_XDOMAIN_CONNECTED is emitted once per link edge,
 *      latched, with no property-generation comparison), and the XDP
 *      properties-changed notification is best-effort. A raced/lost
 *      notification latches a stale generation and every later re-read is
 *      silently dropped.
 *
 *   2. One-shot handshake. Each service driver runs a login/HELLO handshake
 *      that does not re-arm on a soft reconnect:
 *        thunderbolt_net      -> login_sent / login_received / login_retries
 *        thunderbolt_ibverbs  -> native_ready_sent / native_remote_ready /
 *                                native_negotiated / native_ready_attempts
 *      both carried over tb_xdomain_request()/tb_xdomain_response().
 *
 * The fix is applied uniformly through this header:
 *   - tb_xdomain_generation_stale(): the generation gate. Contract: a cached
 *     generation of 0 (which callers set on any properties-changed / reconnect
 *     event, or via the core's per-xdomain "rescan" sysfs trigger) forces the
 *     next real block (generation >= 1) to be accepted, so a stale latch can
 *     never strand a peer.
 *   - struct tb_xdomain_handshake + helpers: a common login/HELLO state with
 *     tb_xdomain_handshake_reset(), to be called on every reconnect so the
 *     handshake re-runs instead of staying latched.
 *
 * Pure, dependency-light, and safe to vendor verbatim into out-of-tree users
 * (thunderbolt_ibverbs keeps a byte-identical copy under proto/). Keep the
 * three copies in lockstep.
 */
#ifndef _LINUX_THUNDERBOLT_NEGOTIATION_H
#define _LINUX_THUNDERBOLT_NEGOTIATION_H

#include <linux/types.h>

/**
 * tb_xdomain_generation_stale() - should a freshly-read remote block be dropped?
 * @have_remote: whether we already hold this peer's properties/identity
 * @remote_gen:  generation reported with the just-read block
 * @cached_gen:  generation of the block we currently hold
 *
 * Drop ONLY an exact-duplicate re-read (same generation we already hold);
 * accept everything else.
 *
 * A peer's property generation is monotonic *within a session* -- the remote
 * always answers a properties request with its CURRENT (highest) generation, so
 * a re-read can never carry a generation LOWER than one we already cached while
 * the peer stays up. The generation is therefore strictly-newer in normal
 * operation and `remote_gen == cached_gen` is the only in-session "nothing
 * changed, skip the re-parse" case.
 *
 * A generation that went BACKWARDS can only mean the peer REBOOTED: the local
 * block generation is seeded with get_random_u32() at init (xdomain.c) and only
 * incremented, so a fresh boot reseeds to a new random value that is frequently
 * lower than what a non-rebooted peer still caches. The peer's directory has
 * genuinely changed (new boot, re-registered services), so this MUST be
 * accepted -- the old `remote_gen <= cached_gen` gate silently stranded the peer
 * forever once the best-effort PROPERTIES_CHANGED reset-to-0 was lost (observed
 * on the appmana-002<->018 link: 002 rebooted, 018 dropped every re-read of
 * 002's new ThunderboltIP service). Reconnect-recovery paths still reset
 * @cached_gen to 0 (any non-zero gen then accepted); a first read (!@have_remote)
 * is always accepted. Pure predicate, exercised by KUnit + a userspace harness.
 */
static inline bool tb_xdomain_generation_stale(bool have_remote, u32 remote_gen,
					       u32 cached_gen)
{
	return have_remote && remote_gen == cached_gen;
}

/**
 * struct tb_xdomain_handshake - common login/HELLO negotiation state
 * @request_sent: we have sent our LOGIN/HELLO to the peer
 * @peer_seen:    we have received the peer's LOGIN/HELLO/READY
 * @established:  both directions are complete and data may flow
 * @attempts:     number of request attempts since the last reset
 *
 * Mirrors thunderbolt_net's login_sent/login_received and
 * thunderbolt_ibverbs's native_ready_sent/native_remote_ready/native_negotiated
 * so both can share one re-arm contract.
 */
struct tb_xdomain_handshake {
	bool request_sent;
	bool peer_seen;
	bool established;
	unsigned int attempts;
};

/**
 * tb_xdomain_handshake_reset() - re-arm the handshake on a (soft) reconnect.
 *
 * MUST be called whenever the peer may have restarted its side without a
 * physical link edge: on a properties-changed notification, on local service
 * (re-)registration, and from the core "rescan" trigger. Without this the
 * one-shot login/HELLO never retries and the peer is stranded.
 */
static inline void tb_xdomain_handshake_reset(struct tb_xdomain_handshake *h)
{
	h->request_sent = false;
	h->peer_seen = false;
	h->established = false;
	h->attempts = 0;
}

/** tb_xdomain_handshake_complete() - both halves of the handshake observed. */
static inline bool tb_xdomain_handshake_complete(const struct tb_xdomain_handshake *h)
{
	return h->request_sent && h->peer_seen;
}

/**
 * tb_xdomain_handshake_supersede() - a peer reconnect invalidates a stale
 * established handshake.
 *
 * A fresh inbound LOGIN/HELLO arriving while our handshake is already complete
 * means the peer restarted its side WITHOUT a physical link edge (so the ICM
 * emits no remove event): our "established" state now points at the peer's old,
 * freed rings, and our one-shot work will short-circuit instead of re-confirming
 * -- the peer is stranded. Drop the handshake so both sides re-confirm before
 * data flows again. This is the protocol-level equivalent of thunderbolt_net's
 * LOGOUT-reset teardown (its login_work sends a LOGOUT on reconnect and the peer
 * tears its session down). Returns true if a complete handshake was superseded.
 */
static inline bool tb_xdomain_handshake_supersede(struct tb_xdomain_handshake *h)
{
	if (!tb_xdomain_handshake_complete(h))
		return false;
	tb_xdomain_handshake_reset(h);
	return true;
}

/**
 * tb_xdomain_session_zombie() - is an established session dead on the wire?
 * @h: the service's login/HELLO handshake state
 * @paths_active: whether the DMA tunnel backing the session is still
 *		  programmed (software tunnel present AND the routers' hop
 *		  entries still have their enable bits set)
 *
 * The level-triggered revalidation contract, from the appmana-018<->027
 * post-mortem (2026-07): a peer that reboots or re-negotiates WITHOUT a
 * processed hotplug edge leaves the survivor's session "established"
 * (carrier on, handshake latched complete) while the underlying DMA paths
 * are gone -- the routers' lane-adapter hop entries read back enable=0, so
 * every frame is silently dropped at the adapter. Both ends latch complete,
 * so neither ever re-runs its one-shot LOGIN/HELLO: a permanent mutual
 * zombie. The edge-triggered signals upstream relies on (hotplug unplug,
 * LOGOUT) demonstrably do not arrive on this hardware.
 *
 * A service driver MUST therefore periodically evaluate this predicate while
 * its session is established (thunderbolt_net: while carrier is on) and, on
 * true, tear the session down and re-run its normal spec negotiation
 * (LOGIN/HELLO retries). Only an ESTABLISHED session can zombie; a session
 * mid-handshake is governed by the login retry loop and must not be touched.
 */
static inline bool
tb_xdomain_session_zombie(const struct tb_xdomain_handshake *h,
			  bool paths_active)
{
	return tb_xdomain_handshake_complete(h) && !paths_active;
}

/*
 * Service-set negotiation. One XDomain link advertises a SET of protocol
 * services in its property directory; the peer's core enumerates the set and
 * binds a matching service driver to each entry (thunderbolt_net to the stock
 * ThunderboltIP UUID, thunderbolt_ibverbs to the native UUID). The set is
 * dynamic: thunderbolt_ibverbs adds the ThunderboltIP entry only when
 * tbnet_identity != off, so flipping that param (or a reload that flips it)
 * is a property-directory change the peer MUST re-read to (un)bind tbnet.
 *
 * Same failure mode as the property generation above: the change rides the
 * best-effort properties-changed notification with no link edge, so a lost
 * notification leaves the peer's enumerated set stale and the newly-added
 * service never binds (thunderbolt_net loads then unloads -- no tbX appears).
 * Shared by the advertiser (ibverbs, which must bump generation on a set change)
 * and the binder (tbnet, which must (re)bind on a newly-present entry).
 */
enum {
	TB_XSVC_NATIVE = 1u << 0,	/* thunderbolt_ibverbs native data */
	TB_XSVC_TBNET  = 1u << 1,	/* stock ThunderboltIP / thunderbolt_net */
};

/**
 * tb_xdomain_services_added() - services present now but not before.
 * @prev: the service mask the peer had previously enumerated
 * @now:  the service mask in the freshly-read property block
 *
 * The core must bind a service driver for each bit returned here. Pure helper
 * so the enumerate path and the KUnit/userspace mirrors share one rule.
 */
static inline u32 tb_xdomain_services_added(u32 prev, u32 now)
{
	return now & ~prev;
}

/**
 * tb_xdomain_services_removed() - services gone now that were present before.
 * The core must unbind a service driver for each bit returned here.
 */
static inline u32 tb_xdomain_services_removed(u32 prev, u32 now)
{
	return prev & ~now;
}

/**
 * tb_xdomain_service_set_changed() - did the LOCAL advertised set change?
 *
 * The advertiser calls this when its service set is (re)computed (e.g. on a
 * tbnet_identity change). Contract: a true result MUST bump the local property
 * generation, so the peer's tb_xdomain_generation_stale() gate accepts the
 * re-read and re-enumerates -- otherwise an unchanged generation strands the
 * new/removed service exactly like a stale handshake strands a peer.
 */
static inline bool tb_xdomain_service_set_changed(u32 prev_local, u32 now_local)
{
	return prev_local != now_local;
}

/*
 * ============================================================================
 * Installable cohesive negotiation
 * ============================================================================
 * The predicates above are the building blocks; these macros compose them into
 * drop-in state + operations so thunderbolt_ibverbs and thunderbolt_net "install"
 * the SAME negotiation into their own per-link objects. Because both sides run
 * byte-identical logic, a service-set change advertised by one driver is honoured
 * by the peer's other driver -- that is the cohesion: tbnet binds iff ibverbs
 * advertised it, through the same gate and the same forced-rescan-on-reconnect.
 *
 * A driver:
 *   1. embeds TB_XNEG_STATE; in its per-peer struct,
 *   2. TB_XNEG_INIT() on connect, TB_XNEG_RECONNECT() on every soft reconnect
 *      (service reload / properties-changed / rescan trigger),
 *   3. TB_XNEG_ADVERTISE() whenever it recomputes its local service set
 *      (e.g. a tbnet_identity flip adds/removes TB_XSVC_TBNET),
 *   4. TB_XNEG_ENUMERATE() on each notification/poll, passing _bind/_unbind
 *      statements that (un)attach the per-service driver (tbnet netdev, ibverbs
 *      rail) for the bits in `svc`.
 * The forced-rescan latch makes step 4 re-read even when the best-effort
 * properties-changed notification was lost -- the bug this prevents is the peer
 * never re-enumerating after a soft reconnect, stranding the new service.
 */
#define TB_XNEG_STATE						\
	struct tb_xdomain_handshake xneg_hs;			\
	bool xneg_have_remote;					\
	u32  xneg_cached_gen;					\
	u32  xneg_remote_svcs;					\
	u32  xneg_bound_svcs;					\
	u32  xneg_local_svcs;					\
	u32  xneg_local_gen;					\
	bool xneg_force_rescan

#define TB_XNEG_INIT(o, local_services) do {			\
	tb_xdomain_handshake_reset(&(o)->xneg_hs);		\
	(o)->xneg_have_remote = false;				\
	(o)->xneg_cached_gen = 0;				\
	(o)->xneg_remote_svcs = 0;				\
	(o)->xneg_bound_svcs = 0;				\
	(o)->xneg_local_svcs = (local_services);		\
	(o)->xneg_local_gen = 1;				\
	(o)->xneg_force_rescan = true;				\
} while (0)

/* a soft reconnect: re-arm the handshake AND force the next directory re-read */
#define TB_XNEG_RECONNECT(o) do {				\
	tb_xdomain_handshake_reset(&(o)->xneg_hs);		\
	(o)->xneg_force_rescan = true;				\
} while (0)

/* local service set (re)computed; bump the generation on a real change so the
 * peer's tb_xdomain_generation_stale() gate accepts the re-read */
#define TB_XNEG_ADVERTISE(o, services) do {			\
	if (tb_xdomain_service_set_changed((o)->xneg_local_svcs, (services))) { \
		(o)->xneg_local_svcs = (services);		\
		(o)->xneg_local_gen++;				\
	}							\
} while (0)

/* re-read the peer directory through the gate (forced after a reconnect) and
 * run _bind/_unbind with `u32 svc` = the changed-service mask */
#define TB_XNEG_ENUMERATE(o, peer_gen, peer_svcs, _bind, _unbind) do {	\
	u32 __cached = (o)->xneg_force_rescan ? 0u : (o)->xneg_cached_gen; \
	if (!tb_xdomain_generation_stale((o)->xneg_have_remote, (peer_gen), __cached)) { \
		u32 svc;						\
		u32 __add = tb_xdomain_services_added((o)->xneg_remote_svcs, (peer_svcs)); \
		u32 __del = tb_xdomain_services_removed((o)->xneg_remote_svcs, (peer_svcs)); \
		(o)->xneg_have_remote = true;			\
		(o)->xneg_cached_gen = (peer_gen);		\
		(o)->xneg_remote_svcs = (peer_svcs);		\
		(o)->xneg_force_rescan = false;			\
		if (__add) { svc = __add; (o)->xneg_bound_svcs |= __add; { _bind; } } \
		if (__del) { svc = __del; (o)->xneg_bound_svcs &= ~__del; { _unbind; } } \
		(void)svc;					\
	}							\
} while (0)

#define TB_XNEG_BOUND(o, service)  (((o)->xneg_bound_svcs & (service)) == (service))

#endif /* _LINUX_THUNDERBOLT_NEGOTIATION_H */
