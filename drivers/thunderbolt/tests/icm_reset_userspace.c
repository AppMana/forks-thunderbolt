// SPDX-License-Identifier: GPL-2.0
/*
 * Userspace mirror of the ICM unload state-machine KUnit (test.c). Models the
 * Alpine/Titan Ridge firmware lifecycle across a driver rmmod+modprobe, with
 * enough fidelity to reproduce BOTH failure modes we have seen on hardware:
 *
 *   1. The -110 wedge: DRV_UNLOADS leaves an AR/TR firmware "running" but
 *      unresponsive (REG_FW_STS_ICM_EN set), so the next driver_ready times out
 *      ("failed to send driver ready to ICM"). icm_stop() now issues a
 *      CIO/firmware reset to clear it.
 *   2. The "ICM firmware not authenticated" failure (observed 2026-06-17 on 002):
 *      the CIO reset DE-AUTHENTICATES the firmware (clears
 *      REG_FW_STS_NVM_AUTH_DONE), and re-authentication takes a cold-boot-length
 *      time. The reload's get_mode auth wait is short, and firmware_start does
 *      NOT wait when the firmware is already "running" -- so if icm_stop resets
 *      and returns immediately, the reload comes up with the firmware running
 *      but not yet authenticated -> no usb4_rdma. The fix: icm_stop must wait for
 *      the firmware to come back running AND authenticated before completing the
 *      unload (icm_firmware_reauth_complete()).
 *
 * No test-only toggles: the model drives the real production predicates
 * (icm_stop_should_reset_firmware, icm_firmware_reauth_complete). Reverting a
 * predicate reproduces the corresponding failure; commenting the auth check out
 * of icm_firmware_reauth_complete() turns the live-reload case red. Keep in
 * lockstep with the KUnit in test.c.
 *
 *   cc -o /tmp/icm icm_reset_userspace.c && /tmp/icm
 */
#include <stdbool.h>
#include <stdio.h>
#include "../icm_reset.h"

static int failures;
#define CHECK(c, m) do { \
	printf("  [%s] %s\n", (c) ? "ok" : "FAIL", m); \
	if (!(c)) failures++; \
} while (0)

/* Re-authentication takes this many ticks after a CIO/firmware reset. */
#define FW_AUTH_TICKS		8
/* The reload's get_mode auth wait (icm_ar_get_mode) -- short on purpose. */
#define RELOAD_AUTH_BUDGET	3
/* icm_stop's post-reset reauth wait (the fix) -- cold-boot-length. */
#define UNLOAD_AUTH_BUDGET	12

/* Model of the on-controller ICM firmware. */
struct icm_fw {
	bool icm_en;		/* REG_FW_STS_ICM_EN: firmware running */
	bool wedged;		/* running but no longer answers ICM_DRIVER_READY */
	bool authed;		/* REG_FW_STS_NVM_AUTH_DONE */
	int auth_remaining;	/* ticks until re-auth completes (0 => authed) */
};

/* Time passes; in-flight re-authentication progresses. */
static void fw_tick(struct icm_fw *fw, int ticks)
{
	if (fw->auth_remaining > 0) {
		fw->auth_remaining -= ticks;
		if (fw->auth_remaining <= 0) {
			fw->auth_remaining = 0;
			fw->authed = true;
		}
	}
}

static void fw_cold_boot(struct icm_fw *fw)
{
	fw->icm_en = true;	/* firmware running + authenticated after power-on */
	fw->wedged = false;
	fw->authed = true;
	fw->auth_remaining = 0;
}

/* NHI_MAILBOX_DRV_UNLOADS: an AR/TR firmware stays running but stops answering. */
static void fw_drv_unloads(struct icm_fw *fw, bool ar_tr_family)
{
	if (fw->icm_en && ar_tr_family)
		fw->wedged = true;
}

/* icm_firmware_reset(): CIO reset + ARC restart. Clears the wedge, but
 * de-authenticates the NVM and starts a fresh (slow) re-authentication. */
static void fw_firmware_reset(struct icm_fw *fw)
{
	fw->icm_en = true;
	fw->wedged = false;
	fw->authed = false;
	fw->auth_remaining = FW_AUTH_TICKS;
}

/* icm_firmware_start(): resets only when NOT running, and (when it does) waits
 * briefly for auth. A firmware that is already "running" is left as-is. */
static void fw_firmware_start(struct icm_fw *fw)
{
	if (!fw->icm_en) {
		fw_firmware_reset(fw);
		fw_tick(fw, RELOAD_AUTH_BUDGET);
	}
}

/* icm_ar_get_mode(): waits up to RELOAD_AUTH_BUDGET for NVM_AUTH_DONE; if the
 * firmware is still not authenticated it fails ("firmware not authenticated"). */
static bool fw_get_mode_ok(struct icm_fw *fw)
{
	int b = RELOAD_AUTH_BUDGET;

	while (b-- > 0 && !fw->authed)
		fw_tick(fw, 1);
	return fw->authed;
}

/* icm_driver_ready() on the reload: firmware_init (start + get_mode) then the
 * ICM_DRIVER_READY handshake. Succeeds iff running, authenticated, not wedged. */
static bool fw_driver_ready(struct icm_fw *fw)
{
	fw_firmware_start(fw);
	if (!fw_get_mode_ok(fw))
		return false;			/* "ICM firmware not authenticated" */
	return fw->icm_en && !fw->wedged;	/* ICM_DRIVER_READY (no -110) */
}

/*
 * icm_stop() driven by the real production predicates. ar_tr/have_upstream/
 * going_away decide whether to reset; after a reset we wait for the firmware to
 * come back ready (running AND authenticated) before completing the unload.
 */
static void icm_stop_model(struct icm_fw *fw, bool ar_tr, bool have_upstream,
			   bool going_away)
{
	fw_drv_unloads(fw, ar_tr);
	if (icm_stop_should_reset_firmware(ar_tr, have_upstream, going_away)) {
		int budget = UNLOAD_AUTH_BUDGET;

		fw_firmware_reset(fw);
		/* The fix: leave the firmware running AND re-authenticated, so the
		 * reload does not see "ICM firmware not authenticated". */
		while (budget-- > 0 &&
		       !icm_firmware_reauth_complete(fw->icm_en, fw->authed))
			fw_tick(fw, 1);
	}
}

int main(void)
{
	struct icm_fw fw;

	printf("icm unload state machine (wedge + re-auth):\n");

	/* Cold boot then load: driver_ready works. */
	fw_cold_boot(&fw);
	CHECK(fw_driver_ready(&fw), "cold boot -> driver_ready succeeds");

	/*
	 * -110 wedge prevention: a Titan Ridge rmmod resets on unload, so the
	 * reload does not time out. (icm_stop_should_reset_firmware governs this.)
	 */
	fw_cold_boot(&fw);
	icm_stop_model(&fw, /*ar_tr=*/true, /*upstream=*/true, /*going_away=*/false);
	CHECK(!fw.wedged, "Titan Ridge rmmod -> reset clears the -110 wedge");

	/*
	 * THE OBSERVED BUG (002, 2026-06-17): after the reset-on-unload the reload
	 * must still come up authenticated. If icm_stop returns before the firmware
	 * re-authenticates, the reload's short get_mode wait expires and
	 * driver_ready fails "firmware not authenticated". The fix is for icm_stop
	 * to wait until icm_firmware_reauth_complete(); drop the auth check from
	 * that predicate and this line goes red, reproducing the failure.
	 */
	fw_cold_boot(&fw);
	icm_stop_model(&fw, /*ar_tr=*/true, /*upstream=*/true, /*going_away=*/false);
	CHECK(fw_driver_ready(&fw),
	      "Titan Ridge rmmod -> reload comes up AUTHENTICATED (driver_ready clean)");

	/*
	 * Not rigged: a USB4/Maple Ridge controller never resets (no cio_reset) and
	 * never wedges, so its reload is clean regardless of the auth machinery.
	 */
	fw_cold_boot(&fw);
	icm_stop_model(&fw, /*ar_tr=*/false, /*upstream=*/true, /*going_away=*/false);
	CHECK(fw_driver_ready(&fw),
	      "Maple Ridge rmmod -> reload driver_ready clean (no reset, no wedge)");

	printf("%s\n", failures ? "FAILED" : "PASSED");
	return failures ? 1 : 0;
}
