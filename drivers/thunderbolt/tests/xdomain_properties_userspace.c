// SPDX-License-Identifier: GPL-2.0
/*
 * Userspace harness for tb_xdomain_generation_stale() and the matching KUnit
 * cases in test.c. Fleet kernels lack CONFIG_KUNIT, so this runs the same truth
 * table with plain `cc`. It is NOT a hand-copied "mirror": it #includes the ONE
 * real shared header so the predicate cannot drift -- single source of truth for
 * the core, thunderbolt_net and thunderbolt_ibverbs.
 *
 *   cc -o /tmp/xdprop xdomain_properties_userspace.c && /tmp/xdprop
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef uint32_t u32; /* the header's kernel type, for the userspace build */
#include "../thunderbolt_negotiation.h"

#define CHECK(expr, want) do {                                               \
	bool got = (expr);                                                   \
	printf("  %-55s => %-5s [%s]\n", #expr, got ? "true" : "false",      \
	       got == (want) ? "ok" : "FAIL");                               \
	if (got != (want)) failures++;                                       \
} while (0)

int main(void)
{
	int failures = 0;
	printf("tb_xdomain_generation_stale truth table:\n");

	/* first read: always accept (not stale) */
	CHECK(tb_xdomain_generation_stale(false, 1, 0), false);
	CHECK(tb_xdomain_generation_stale(false, 7, 0), false);
	/* strictly newer: accept */
	CHECK(tb_xdomain_generation_stale(true, 8, 5), false);
	/* exact duplicate (same gen we hold): drop, skip the re-parse */
	CHECK(tb_xdomain_generation_stale(true, 5, 5), true);
	/* lower gen = the peer REBOOTED (random reseed): accept, do NOT strand it */
	CHECK(tb_xdomain_generation_stale(true, 5, 7), false);
	/* reconnect-recovery: cached gen reset to 0 forces accept of any real block */
	CHECK(tb_xdomain_generation_stale(true, 5, 0), false);
	CHECK(tb_xdomain_generation_stale(true, 1, 0), false);

	/*
	 * THE FLEET BUG, now FIXED (appmana-002<->018, 2026-06-16).
	 * xdomain_property_block_gen is seeded by get_random_u32() at init
	 * (xdomain.c) and only incremented. So when a peer REBOOTS its generation
	 * reseeds to a NEW RANDOM value frequently LOWER than the one a NON-rebooted
	 * peer still caches. The old monotonic gen<=cached gate silently dropped
	 * every re-read -> the peer was stranded forever once the best-effort
	 * PROPERTIES_CHANGED reset-to-0 was lost (002 rebooted; 018 never
	 * re-enumerated 002's thunderbolt_net service). The gate now drops ONLY an
	 * exact-duplicate re-read; a lower generation can only mean a reboot, so it
	 * is accepted and the peer re-enumerates.
	 */
	printf("\nreboot/restart scenario (the fixed 002<->018 stranding):\n");
	{
		const uint32_t cached_old = 0xC0FFEE99u; /* cached before peer rebooted */
		const uint32_t gen_new    = 0x0000002Au; /* post-reboot random gen, lower */

		/* the rebooted peer's lower-gen block is now ACCEPTED, not stranded */
		CHECK(tb_xdomain_generation_stale(true, gen_new, cached_old), false);
		/* a true duplicate (same gen) is still dropped */
		CHECK(tb_xdomain_generation_stale(true, cached_old, cached_old), true);
	}

	printf("%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
	return failures ? 1 : 0;
}
