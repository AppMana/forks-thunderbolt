// SPDX-License-Identifier: GPL-2.0
/*
 * Userspace mirror of the tbnet-bind renegotiation KUnit. Adversarial model of
 * why thunderbolt_net "loads then unloads, no tbX iface appears" after a
 * coordinated soft reload that flips tbnet_identity=stock:
 *
 *   thunderbolt_ibverbs adds TB_XSVC_TBNET to its property directory and bumps
 *   its generation, but the properties-changed notification is best-effort and a
 *   soft reload (no physical link edge) can drop it. If the peer only re-reads
 *   the directory ON a notification, it never re-enumerates and never binds
 *   thunderbolt_net to the new service. The fix: a reconnect FORCES a rescan
 *   instead of trusting the notification.
 *
 * Both drivers install the SAME negotiation via the macros in
 * thunderbolt_negotiation.h, so this models them cohesively: a service the
 * ibverbs side advertises is bound by the tbnet side iff the shared rescan fires.
 *
 *   cc -I.. -o /tmp/tbnet_bind tbnet_bind_userspace.c && /tmp/tbnet_bind
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t u32;
#include "../../../thunderbolt/thunderbolt_negotiation.h"

static int failures;
#define CHECK(c, m) do { \
	printf("  [%s] %s\n", (c) ? "ok" : "FAIL", m); \
	if (!(c)) failures++; \
} while (0)

/* a per-peer object in EITHER driver -- installs the shared negotiation state */
struct link_peer { TB_XNEG_STATE; };

/*
 * The driver's poll/work: re-read the peer's directory iff a notification
 * arrived OR a reconnect forced a rescan, binding/unbinding the per-service
 * driver (here just recording the bind in xneg_bound_svcs via the macro).
 */
static void poll_peer(struct link_peer *self, const struct link_peer *peer,
		      bool notified)
{
	if (!(notified || self->xneg_force_rescan))
		return;
	TB_XNEG_ENUMERATE(self, peer->xneg_local_gen, peer->xneg_local_svcs,
			  /*_bind*/   (void)svc,
			  /*_unbind*/ (void)svc);
}

/*
 * A soft reload that flips tbnet_identity=stock: re-arm the handshake and add
 * the ThunderboltIP service. `fix` selects whether the reconnect forces a
 * rescan (the fix) or merely re-arms and trusts the notification (the bug). The
 * best-effort notification to the peer is LOST on this soft reload.
 */
static void reload_add_tbnet(struct link_peer *self, bool fix)
{
	if (fix)
		TB_XNEG_RECONNECT(self);		/* the fix: force a rescan */
	else
		tb_xdomain_handshake_reset(&self->xneg_hs); /* old: re-arm only */
	TB_XNEG_ADVERTISE(self, TB_XSVC_NATIVE | TB_XSVC_TBNET);
}

/* returns whether BOTH peers bound thunderbolt_net after the reload */
static bool run(bool fix)
{
	struct link_peer a, b;

	TB_XNEG_INIT(&a, TB_XSVC_NATIVE);
	TB_XNEG_INIT(&b, TB_XSVC_NATIVE);

	/* initial connect: both enumerate and bind the native service */
	poll_peer(&a, &b, true);
	poll_peer(&b, &a, true);

	/* coordinated reload flipping tbnet_identity=stock; notifications LOST */
	reload_add_tbnet(&a, fix);
	reload_add_tbnet(&b, fix);

	/* poll cycles with notifications that never arrive */
	for (int i = 0; i < 10; i++) {
		poll_peer(&a, &b, /*notified=*/false);
		poll_peer(&b, &a, /*notified=*/false);
	}
	return TB_XNEG_BOUND(&a, TB_XSVC_TBNET) && TB_XNEG_BOUND(&b, TB_XSVC_TBNET);
}

int main(void)
{
	printf("tbnet-bind renegotiation model:\n");
	/* sanity: native binds on the initial connect regardless */
	{
		struct link_peer a, b;
		TB_XNEG_INIT(&a, TB_XSVC_NATIVE);
		TB_XNEG_INIT(&b, TB_XSVC_NATIVE);
		poll_peer(&a, &b, true);
		poll_peer(&b, &a, true);
		CHECK(TB_XNEG_BOUND(&a, TB_XSVC_NATIVE) &&
		      TB_XNEG_BOUND(&b, TB_XSVC_NATIVE), "native binds on connect");
		CHECK(!TB_XNEG_BOUND(&a, TB_XSVC_TBNET), "tbnet absent before the flip");
	}
	CHECK(run(false) == false,
	      "WITHOUT forced rescan: lost notify strands tbnet (BUG reproduced)");
	CHECK(run(true) == true,
	      "WITH forced rescan: tbnet binds on both peers (FIX)");
	printf("%s\n", failures ? "FAILED" : "PASSED");
	return failures ? 1 : 0;
}
