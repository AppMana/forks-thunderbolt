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
 * Normally only a strictly-newer generation is accepted. Reconnect-recovery
 * paths reset @cached_gen to 0 so any real block (generation >= 1) is accepted
 * again; a first read (!@have_remote) is always accepted. Pure predicate so it
 * can be exercised by KUnit and a userspace mirror.
 */
static inline bool tb_xdomain_generation_stale(bool have_remote, u32 remote_gen,
					       u32 cached_gen)
{
	return have_remote && remote_gen <= cached_gen;
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

#endif /* _LINUX_THUNDERBOLT_NEGOTIATION_H */
