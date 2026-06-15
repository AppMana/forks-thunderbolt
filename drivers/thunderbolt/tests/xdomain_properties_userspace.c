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

	printf("%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
	return failures ? 1 : 0;
}
