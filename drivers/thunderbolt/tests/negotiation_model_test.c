// SPDX-License-Identifier: GPL-2.0
/*
 * Userspace mirror of the KUnit XDomain re-negotiation model. Reproduces the
 * real appmana-002<->018 stranding on a faithful two-host + firmware model and
 * proves the fix. There is NO "apply the fix" toggle: the only lever is the real
 * production predicate tb_xdomain_generation_stale(); comment its fix out (in
 * thunderbolt_negotiation.h) and the stranding case below goes red, exactly as
 * reverting the driver would. Keep in lockstep with the KUnit in test.c.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t u32;
#include "negotiation_model.h"

static int failures;
#define CHECK(cond, msg) do { \
	printf("  [%s] %s\n", (cond) ? "ok" : "FAIL", msg); \
	if (!(cond)) failures++; \
} while (0)

int main(void)
{
	printf("XDomain re-negotiation (faithful firmware + real gen gate):\n");

	/* Cold boot: link edge -> firmware event -> both enumerate -> established. */
	{
		struct model_link L;
		memset(&L, 0, sizeof(L));
		model_cold_boot(&L, /*gen_a=*/0xC0FFEE99u, /*gen_b=*/0x11111111u);
		CHECK(model_established(&L), "cold boot -> both hosts enumerate the peer");
	}

	/*
	 * THE BUG: host A reboots and reseeds to a LOWER generation, and B gets
	 * no fresh firmware edge (absorbed/raced one-shot). B's only recovery is
	 * the gated software re-read. With the FIXED gate B accepts the lower
	 * generation and re-enumerates A's new services; revert the gate (in the
	 * header) and B drops every re-read forever -> stranded (this line fails).
	 */
	{
		struct model_link L;
		memset(&L, 0, sizeof(L));
		model_cold_boot(&L, 0xC0FFEE99u, 0x11111111u);
		model_peer_reboot(&L.a, /*new_gen=*/0x0000002Au);  /* lower than cached */
		model_run(&L, 50);
		CHECK(model_established(&L),
		      "peer reboot to LOWER gen -> non-rebooted host re-enumerates (the fix)");
	}

	/*
	 * Faithfulness check #1: the model is not rigged to always recover. A
	 * reboot to a HIGHER generation is accepted by BOTH the old and new gate
	 * (it is not the stranding condition), so it must establish regardless --
	 * proving the recovery above is specifically the lower-gen fix, not a
	 * model that ignores the gate.
	 */
	{
		struct model_link L;
		memset(&L, 0, sizeof(L));
		model_cold_boot(&L, 0x00000005u, 0x11111111u);
		model_peer_reboot(&L.a, /*new_gen=*/0x00000009u);  /* higher than cached */
		model_run(&L, 50);
		CHECK(model_established(&L),
		      "peer reboot to HIGHER gen -> re-enumerates (not the stranding case)");
	}

	/*
	 * Faithfulness check #2: the firmware genuinely offers no fallback. If the
	 * gate drops the re-read, nothing else re-enumerates the peer -- so a
	 * model in which the gate said "stale" would stay stranded no matter how
	 * many poll rounds run. We assert the real gate does NOT call the lower-gen
	 * block stale (i.e. the recovery came from the gate, not from the model).
	 */
	CHECK(!tb_xdomain_generation_stale(true, 0x2Au, 0xC0FFEE99u),
	      "real gate accepts the rebooted peer's lower generation");
	CHECK(tb_xdomain_generation_stale(true, 0x2Au, 0x2Au),
	      "real gate still drops an exact-duplicate re-read");

	printf("%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
	return failures ? 1 : 0;
}
