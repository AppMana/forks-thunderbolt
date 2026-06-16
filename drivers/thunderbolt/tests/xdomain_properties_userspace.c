// SPDX-License-Identifier: GPL-2.0
/*
 * Userspace mirror of tb_xdomain_generation_stale() (tb.h) and the KUnit case
 * tb_test_xdomain_properties_stale() (test.c). Fleet kernels lack CONFIG_KUNIT,
 * so this asserts the same truth table for the generation gate + reload fix.
 * Keep in lockstep with the kernel helper.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* MIRROR of tb.h: tb_xdomain_generation_stale() */
static inline bool tb_xdomain_generation_stale(bool have_remote, uint32_t gen,
					       uint32_t cached_gen)
{
	return have_remote && gen <= cached_gen;
}

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
	/* THE BUG: equal/non-advanced gen dropped */
	CHECK(tb_xdomain_generation_stale(true, 5, 5), true);
	CHECK(tb_xdomain_generation_stale(true, 5, 7), true);
	/* THE FIX: cached gen reset to 0 forces accept */
	CHECK(tb_xdomain_generation_stale(true, 5, 0), false);
	CHECK(tb_xdomain_generation_stale(true, 1, 0), false);

	/*
	 * THE OBSERVED FLEET BUG (appmana-002<->018, 2026-06-16, both on the tbfix
	 * core). xdomain_property_block_gen is seeded by get_random_u32() at init
	 * (xdomain.c:2622) and only incremented (:2568). So when a peer REBOOTS its
	 * generation reseeds to a NEW RANDOM value that is frequently LOWER than the
	 * one a NON-rebooted peer still caches. The rebooted peer's directory
	 * genuinely CHANGED (it re-registers "network"/tbverbs/...), but the
	 * monotonic gen<=cached gate silently drops every re-read once the
	 * best-effort PROPERTIES_CHANGED reset-to-0 is lost -> the peer is stranded
	 * FOREVER. Observed: 002 rebooted, so 018 (not rebooted) kept a stale-high
	 * cached gen for 002 and never re-enumerated 002's thunderbolt_net service,
	 * while 002 (no cached gen for 018) saw 018 fine. Directional + deterministic.
	 *
	 * The gate cannot tell a stale duplicate from a restart-with-lower-gen from
	 * (gen, cached) alone: a re-read whose remote property BLOCK changed (a
	 * restart) MUST be accepted regardless of gen direction. The current helper
	 * has no such signal, so these model the FIX REQUIREMENT and FAIL today.
	 */
	printf("\nreboot/restart scenario (the observed 002<->018 stranding):\n");
	{
		const uint32_t cached_old = 0xC0FFEE99u; /* cached before peer rebooted */
		const uint32_t gen_new    = 0x0000002Au; /* post-reboot random gen, lower */

		/* current behaviour: the restarted peer's changed block is DROPPED */
		CHECK(tb_xdomain_generation_stale(true, gen_new, cached_old), true);

		/*
		 * REQUIRED behaviour: a re-read whose remote block CHANGED must be
		 * accepted even when the gen went backwards. Modelled against the
		 * current gen-only helper, so it FAILS until the helper learns the
		 * "remote block changed / restart" signal. want=false (must accept);
		 * the gen-only gate returns true -> failing case = the bug reproduced.
		 */
		const bool remote_block_changed = true; /* peer re-registered services */
		bool dropped = tb_xdomain_generation_stale(true, gen_new, cached_old);
		bool accept_required = remote_block_changed; /* restart => must accept */
		CHECK(dropped && accept_required /* dropped a genuine restart? */, false);
	}

	printf("%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
	return failures ? 1 : 0;
}
