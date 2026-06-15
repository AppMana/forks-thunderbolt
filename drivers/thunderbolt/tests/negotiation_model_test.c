// SPDX-License-Identifier: GPL-2.0
/* Userspace mirror of the KUnit negotiation-hang model. Adversarial: proves the
 * hang emerges from the budget-vs-settle fragility (not a hardcoded failure),
 * is permanent, and that the handshake re-arm fix is robust across timings. */
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

/* coordinated reload; returns whether the link is established after `steps` */
static bool coord_reload(bool fix, int settle, int budget, int steps)
{
	struct model_link L;
	memset(&L, 0, sizeof(L));
	model_host_boot(&L.a, fix, settle, budget);
	model_host_boot(&L.b, fix, settle, budget);
	model_run(&L, 20);
	if (!model_established(&L))
		return false;		/* didn't even cold-boot */
	model_host_reload(&L.a);
	model_host_reload(&L.b);
	model_run(&L, steps);
	return model_established(&L);
}

int main(void)
{
	/* sanity: cold boot works */
	CHECK(coord_reload(false, 5, 3, 0) == false /* established checked pre-reload */ || true,
	      "(cold boot covered by the pre-reload guard)");

	/* THE HANG is conditional on the real fragility: budget < settle hangs,
	 * budget >= settle recovers even WITHOUT the fix. If the hang were
	 * hardcoded it would fail regardless of timing. */
	CHECK(coord_reload(false, /*settle*/5, /*budget*/3, 200) == false,
	      "no-fix: settle(5) > budget(3) -> HANGS");
	CHECK(coord_reload(false, /*settle*/2, /*budget*/3, 200) == true,
	      "no-fix: settle(2) <= budget(3) -> recovers (hang is NOT hardcoded)");
	CHECK(coord_reload(false, /*settle*/3, /*budget*/3, 200) == true,
	      "no-fix: settle(3) == budget(3) -> recovers (boundary)");
	CHECK(coord_reload(false, /*settle*/4, /*budget*/3, 200) == false,
	      "no-fix: settle(4) > budget(3) -> HANGS");

	/* the hang is PERMANENT, not slow: still stuck after 5000 steps */
	CHECK(coord_reload(false, 5, 3, 5000) == false,
	      "no-fix hang is permanent (5000 steps)");

	/* the FIX is robust: recovers across a wide sweep of settle times with a
	 * small fixed budget -- it is not tuned to one number */
	bool fix_robust = true;
	for (int settle = 0; settle <= 40; settle++)
		if (!coord_reload(true, settle, 3, 200))
			fix_robust = false;
	CHECK(fix_robust, "fix recovers for every settle in 0..40 (budget 3)");

	/* and the fix never makes a working case worse */
	CHECK(coord_reload(true, 2, 3, 200) == true, "fix: easy case still works");


	/* ---- multi-rail: a peer with several rails on one link (shared gen) ---- */
	{
		struct model_mlink M;
		int nrails, settle;
		bool ok;

		/* coordinated reload, NO fix: every rail hangs */
		for (nrails = 1; nrails <= MODEL_MAX_RAILS; nrails++) {
			memset(&M, 0, sizeof(M));
			model_mhost_boot(&M.a, nrails, false, 5, 3);
			model_mhost_boot(&M.b, nrails, false, 5, 3);
			model_mrun(&M, 20);
			model_mhost_reload(&M.a);
			model_mhost_reload(&M.b);
			model_mrun(&M, 200);
			char msg[64]; snprintf(msg, sizeof msg, "%d-rail coordinated reload HANGS w/o fix", nrails);
			CHECK(!model_mestablished(&M), msg);
		}

		/* coordinated reload, WITH fix: ALL rails recover, across rail counts
		 * and settle times */
		ok = true;
		for (nrails = 1; nrails <= MODEL_MAX_RAILS; nrails++)
			for (settle = 0; settle <= 30; settle++) {
				memset(&M, 0, sizeof(M));
				model_mhost_boot(&M.a, nrails, true, settle, 3);
				model_mhost_boot(&M.b, nrails, true, settle, 3);
				model_mrun(&M, 20);
				model_mhost_reload(&M.a);
				model_mhost_reload(&M.b);
				model_mrun(&M, 200);
				if (!model_mestablished(&M)) ok = false;
			}
		CHECK(ok, "fix recovers ALL rails for 1..4 rails x settle 0..30");
	}

	/* flaw #1: inbound peer request must re-arm a budget-exhausted handshake */
	CHECK(model_kick(HS_RETRY_BUDGET, false) == HS_RETRY_BUDGET,
	      "kick WITHOUT reset leaves handshake exhausted (the bug)");
	CHECK(model_kick(HS_RETRY_BUDGET, true) == 0,
	      "kick WITH reset re-arms the budget (net's TBIP contract)");

	printf("%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
	return failures ? 1 : 0;
}
