// SPDX-License-Identifier: GPL-2.0
/*
 * Userspace mirror of the thunderbolt_net login-handshake KUnit. Models net's
 * ThunderboltIP login flow and the soft-reconnect contract that adopting
 * struct tb_xdomain_handshake (the header shared with the core and
 * thunderbolt_ibverbs) brings:
 *
 *   An inbound re-LOGIN arriving while we still believe we are connected means
 *   the peer restarted its side WITHOUT a physical link edge, so our session
 *   points at its freed rings. We MUST supersede (drop the handshake) and
 *   re-confirm before declaring carrier up again -- the protocol-level
 *   equivalent of net's own LOGOUT-on-reconnect teardown.
 *
 * The current net code tracks this with ad-hoc login_sent/login_received
 * booleans and an inbound LOGIN just re-sets login_received, so it keeps
 * reporting connected into a dead tunnel. This test asserts the ADOPTED
 * behaviour, so it FAILS while net still uses the booleans and passes once net
 * uses the shared handshake. Keep in lockstep with the KUnit in main.c.
 *
 *   cc -o /tmp/tbnet_login tbnet_login_userspace.c && /tmp/tbnet_login
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef uint32_t u32;
#include "../../../thunderbolt/thunderbolt_negotiation.h"

static int failures;
#define CHECK(c, m) do { \
	printf("  [%s] %s\n", (c) ? "ok" : "FAIL", m); \
	if (!(c)) failures++; \
} while (0)

/*
 * Does net still report "connected" after a restarted peer re-LOGINs?
 * @adopted: model the shared-handshake net (true) or the current ad-hoc net.
 * The contract: it must NOT (the stale session is dropped, re-confirm first).
 */
static bool net_connected_after_peer_relogin(bool adopted)
{
	if (adopted) {
		struct tb_xdomain_handshake hs = { 0 };

		/* we are logged in and connected */
		hs.request_sent = true;
		hs.peer_seen = true;
		hs.established = true;
		/* restarted peer re-LOGINs: supersede our stale session */
		tb_xdomain_handshake_supersede(&hs);
		return tb_xdomain_handshake_complete(&hs);
	} else {
		/* current net: connected == login_sent && login_received */
		bool login_sent = true, login_received = true;

		/* inbound re-LOGIN: net just re-sets login_received, no teardown */
		login_received = true;
		return login_sent && login_received;	/* stays connected -> stale */
	}
}

int main(void)
{
	printf("thunderbolt_net login-handshake adoption:\n");
	/* current ad-hoc net keeps a stale session up -> this FAILS until adoption */
	CHECK(net_connected_after_peer_relogin(false) == false,
	      "net drops a stale session on a peer re-login");
	/* the adopted (shared handshake) net supersedes it correctly */
	CHECK(net_connected_after_peer_relogin(true) == false,
	      "adopted net supersedes the stale session (reference)");
	printf("%s\n", failures ? "FAILED" : "PASSED");
	return failures ? 1 : 0;
}
