// SPDX-License-Identifier: GPL-2.0
/*
 * Userspace mirror reproducing the re-HELLO supersede bug -- the XDomain
 * re-negotiation hang -- against REAL code paths:
 *
 *   - tbv_rail_data_ready()                 (kernel/tbv.h:336, mirrored verbatim)
 *   - struct tb_xdomain_handshake + helpers (kernel/thunderbolt_negotiation.h,
 *                                            mirrored verbatim; the shared .h)
 *
 * plus an inline model of tbv_native_control_apply_remote()'s inbound-HELLO
 * path (kernel/native_control.c): refresh the remote hops AND supersede a stale
 * established handshake. BEFORE the fix the supersede call was absent (no
 * LOGOUT/reset step like thunderbolt_net's, tbnet_minimal.c:1002-1012), so after
 * a peer reload the established rail kept reporting data-ready into a dead
 * tunnel -- the second CHECK below FAILED. With the fix it re-confirms.
 *
 * The fleet's Ubuntu generic kernels do not set CONFIG_KUNIT, so this harness
 * runs the same contract on any host with cc. Keep it in lockstep with the
 * KUnit case in reconnect_test.c.
 *
 * build+run: cc -O2 -Wall -o /tmp/reconnect reconnect_userspace.c && /tmp/reconnect
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* --- MIRROR of struct tb_xdomain_handshake + helpers (thunderbolt_negotiation.h) --- */
struct tb_xdomain_handshake {
	bool request_sent;
	bool peer_seen;
	bool established;
	unsigned int attempts;
};

static inline void tb_xdomain_handshake_reset(struct tb_xdomain_handshake *h)
{
	h->request_sent = false;
	h->peer_seen = false;
	h->established = false;
	h->attempts = 0;
}

static inline bool
tb_xdomain_handshake_complete(const struct tb_xdomain_handshake *h)
{
	return h->request_sent && h->peer_seen;
}

static inline bool
tb_xdomain_handshake_supersede(struct tb_xdomain_handshake *h)
{
	if (!tb_xdomain_handshake_complete(h))
		return false;
	tb_xdomain_handshake_reset(h);
	return true;
}

/* --- minimal struct shapes (only the fields tbv_rail_data_ready touches) --- */
enum tbv_backend_type { TBV_BACKEND_NATIVE, TBV_BACKEND_APPLE };
enum tbv_path_state {
	TBV_PATH_NEW,
	TBV_PATH_RING_ALLOCATED,
	TBV_PATH_RING_STARTED,
	TBV_PATH_TUNNEL_ENABLED,
	TBV_PATH_STOPPED,
};
struct tbv_peer { enum tbv_backend_type backend; };
struct tbv_path { enum tbv_path_state state; };
struct tbv_rail {
	struct tbv_peer *peer;
	struct tbv_path path;
	struct tb_xdomain_handshake native_hs;
	bool native_negotiated;
	int remote_transmit_path;
};

/* --- MIRROR of tbv_rail_data_ready() in kernel/tbv.h --- */
static inline bool tbv_rail_data_ready(const struct tbv_rail *rail)
{
	if (!rail || rail->path.state != TBV_PATH_TUNNEL_ENABLED)
		return false;
	if (!rail->peer || rail->peer->backend != TBV_BACKEND_NATIVE)
		return true;
	return tb_xdomain_handshake_complete(&rail->native_hs);
}

/*
 * MIRROR of tbv_native_control_apply_remote()'s inbound-HELLO path
 * (native_control.c): refresh the negotiated hops AND supersede a stale
 * established handshake via the shared cross-driver contract, so a restarted
 * peer is forced to re-confirm before data flows again.
 *
 * BEFORE the fix this function did NOT call tb_xdomain_handshake_supersede();
 * the rail stayed data-ready into a dead tunnel and the second CHECK below
 * FAILED (run `git show` on the fix commit for the red state).
 */
static void apply_remote_inbound_hello(struct tbv_rail *rail,
				       int peer_transmit_path)
{
	rail->native_negotiated = true;
	rail->remote_transmit_path = peer_transmit_path;
	tb_xdomain_handshake_supersede(&rail->native_hs);
}

static int failures;
#define CHECK(cond, msg) do {                          \
	if (cond) {                                    \
		printf("  [ok] %s\n", (msg));          \
	} else {                                       \
		printf("  [FAIL] %s\n", (msg));        \
		failures++;                            \
	}                                              \
} while (0)

int main(void)
{
	struct tbv_peer peer = { .backend = TBV_BACKEND_NATIVE };
	struct tbv_rail rail;

	memset(&rail, 0, sizeof(rail));
	rail.peer = &peer;
	rail.path.state = TBV_PATH_TUNNEL_ENABLED;

	/* a fully established native rail, serving data */
	rail.native_hs.request_sent = true;
	rail.native_hs.peer_seen = true;
	rail.native_hs.established = true;
	rail.native_negotiated = true;
	rail.remote_transmit_path = 5;
	CHECK(tbv_rail_data_ready(&rail),
	      "established native rail is data-ready");

	/*
	 * The peer soft-reloads (no link edge, so no XDomain remove event) and
	 * re-HELLOs with a NEW transmit_path -- its old rings are gone.
	 */
	apply_remote_inbound_hello(&rail, 9);

	/*
	 * Observable contract: a restarted peer means the old tunnel/hops are
	 * dead, so the rail must NOT keep reporting data-ready until it
	 * re-handshakes (mirroring thunderbolt_net's LOGOUT-reset teardown).
	 * Without the supersede fix it stays ready -> this FAILED (the hang).
	 */
	CHECK(!tbv_rail_data_ready(&rail),
	      "REPRO: re-HELLO with changed hops supersedes the stale handshake");

	printf("%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
	return failures ? 1 : 0;
}
