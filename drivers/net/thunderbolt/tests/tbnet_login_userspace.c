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
 * Model net's ThunderboltIP login flow as it now is in main.c after adopting
 * struct tb_xdomain_handshake: login sent -> request_sent, remote login ->
 * peer_seen, connected == complete, and an inbound LOGIN that arrives while
 * already complete supersedes the stale session (main.c then queues
 * disconnect_work; the peer's retries re-establish).
 */
struct net_login { struct tb_xdomain_handshake hs; };

static bool net_connected(const struct net_login *n)
{
	return tb_xdomain_handshake_complete(&n->hs);
}
static void net_send_login(struct net_login *n)   { n->hs.request_sent = true; }
static void net_recv_login(struct net_login *n)   { n->hs.peer_seen = true; }
/* returns true if this inbound LOGIN superseded a live session (-> tear down) */
static bool net_peer_login(struct net_login *n)   { return tb_xdomain_handshake_supersede(&n->hs); }
static void net_reset(struct net_login *n)        { tb_xdomain_handshake_reset(&n->hs); }

int main(void)
{
	struct net_login n = { 0 };

	printf("thunderbolt_net login-handshake adoption:\n");

	/* cold connect: both sides log in -> connected */
	net_send_login(&n);
	net_recv_login(&n);
	CHECK(net_connected(&n), "connected once both logins exchanged");

	/* peer restarts and re-LOGINs while we still think we're connected:
	 * main.c supersedes + tears down rather than carrying a stale session.
	 * (capture once -- net_peer_login() has the supersede side effect) */
	bool superseded = net_peer_login(&n);

	CHECK(superseded, "inbound re-login supersedes the live session");
	CHECK(!net_connected(&n), "no longer connected until re-confirmed");

	/* the tear-down resets, then a fresh handshake reconnects cleanly */
	net_reset(&n);
	net_send_login(&n);
	net_recv_login(&n);
	CHECK(net_connected(&n), "reconnects after a clean re-handshake");

	printf("%s\n", failures ? "FAILED" : "PASSED");
	return failures ? 1 : 0;
}
