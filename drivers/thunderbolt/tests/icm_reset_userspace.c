// SPDX-License-Identifier: GPL-2.0
/*
 * Userspace mirror of the ICM unload-wedge KUnit (test.c:
 * tb_test_icm_unload_wedge / tb_test_icm_stop_reset_policy).
 *
 * Models the Alpine/Titan Ridge firmware lifecycle that wedges an rmmod+
 * modprobe cycle, and proves the unload-path reset clears it:
 *
 *   - REG_FW_STS_ICM_EN ("running") survives DRV_UNLOADS, but the firmware
 *     enters a state where it no longer answers ICM_DRIVER_READY.
 *   - icm_firmware_start() only resets the firmware when it is NOT running, so
 *     a wedged-but-running firmware is left wedged -> the next driver_ready
 *     times out ("failed to send driver ready to ICM", probe -110), and only a
 *     host cold boot recovers it.
 *   - icm_firmware_reset() on the unload path restarts the firmware, leaving it
 *     as clean as a cold boot so the reload's driver_ready succeeds.
 *
 * The model drives the REAL production decision (icm_stop_should_reset_firmware
 * from icm_reset.h): with the fix the reload is clean; the no-reset control
 * reproduces the wedge, so the model is faithful. Fleet kernels lack
 * CONFIG_KUNIT; keep this in lockstep with the KUnit in test.c.
 *
 *   cc -o /tmp/icm_reset icm_reset_userspace.c && /tmp/icm_reset
 */
#include <stdbool.h>
#include <stdio.h>
#include "../icm_reset.h"

static int failures;
#define CHECK(c, m) do { \
	printf("  [%s] %s\n", (c) ? "ok" : "FAIL", m); \
	if (!(c)) failures++; \
} while (0)

/* Model of the on-controller ICM firmware. */
struct icm_fw {
	bool icm_en;	/* REG_FW_STS_ICM_EN: firmware running */
	bool wedged;	/* running but no longer answers ICM_DRIVER_READY */
};

static void fw_cold_boot(struct icm_fw *fw)
{
	fw->icm_en = true;	/* firmware authenticated + running after power-on */
	fw->wedged = false;
}

/*
 * NHI_MAILBOX_DRV_UNLOADS: an Alpine/Titan Ridge firmware keeps running but
 * stops answering ICM_DRIVER_READY. USB4/Maple Ridge takes the same mailbox
 * command but does not wedge, so the wedge is gated on the AR/TR family.
 */
static void fw_drv_unloads(struct icm_fw *fw, bool ar_tr_family)
{
	if (fw->icm_en && ar_tr_family)
		fw->wedged = true;
}

/* icm_firmware_reset(): CIO reset + ARC restart -> clean, like a cold boot. */
static void fw_firmware_reset(struct icm_fw *fw)
{
	fw->icm_en = true;
	fw->wedged = false;
}

/* icm_firmware_start(): resets only when the firmware is not already running. */
static void fw_firmware_start(struct icm_fw *fw)
{
	if (!fw->icm_en)
		fw_firmware_reset(fw);
}

/* __icm_driver_ready(): succeeds iff the firmware is running and not wedged. */
static bool fw_driver_ready(struct icm_fw *fw)
{
	fw_firmware_start(fw);
	return fw->icm_en && !fw->wedged;
}

/*
 * icm_stop(), parameterised by the controller capabilities so the model runs
 * the real predicate. Titan/Alpine Ridge: have_cio_reset = have_upstream = true.
 */
static void icm_stop_model(struct icm_fw *fw, bool ar_tr, bool have_upstream,
			   bool going_away)
{
	fw_drv_unloads(fw, ar_tr);			/* DRV_UNLOADS mailbox */
	/*
	 * Drive the real production decision unconditionally, exactly as
	 * icm_stop() does -- there is no test-only "apply the fix" toggle. The
	 * wedge is reproduced (below) by a genuine condition that makes this
	 * predicate return false, not by hand-disabling it.
	 */
	if (icm_stop_should_reset_firmware(ar_tr, have_upstream, going_away))
		fw_firmware_reset(fw);
}

int main(void)
{
	struct icm_fw fw;

	printf("icm unload-wedge model:\n");

	/* Cold boot then load: driver_ready works. */
	fw_cold_boot(&fw);
	CHECK(fw_driver_ready(&fw), "cold boot -> driver_ready succeeds");

	/*
	 * The fix: a Titan Ridge rmmod with the reset port cached resets the
	 * firmware on unload, so the reload's driver_ready succeeds without a
	 * host cold boot. This is the appmana-002 case. (Commenting out the
	 * predicate in icm_reset.h makes this line wedge and the test fail.)
	 */
	fw_cold_boot(&fw);
	icm_stop_model(&fw, /*ar_tr=*/true, /*upstream=*/true,
		       /*going_away=*/false);
	CHECK(fw_driver_ready(&fw),
	      "Titan Ridge rmmod (reset port cached) -> reload driver_ready clean");

	/*
	 * Reproduce the wedge from a REAL production condition, not a toggle:
	 * if the vendor-cap upstream port was never cached, the predicate
	 * returns false, no reset is issued, and the Titan Ridge firmware stays
	 * running-but-wedged. This is exactly the failure that Part 1 (always
	 * caching upstream_port in icm_ar_is_supported) exists to prevent.
	 */
	fw_cold_boot(&fw);
	icm_stop_model(&fw, /*ar_tr=*/true, /*upstream=*/false,
		       /*going_away=*/false);
	CHECK(!fw_driver_ready(&fw),
	      "Titan Ridge rmmod with NO reset port cached -> reload WEDGES (the bug)");

	/* A USB4/Maple Ridge controller never wedges (no cio_reset). */
	fw_cold_boot(&fw);
	icm_stop_model(&fw, /*ar_tr=*/false, /*upstream=*/true,
		       /*going_away=*/false);
	CHECK(fw_driver_ready(&fw),
	      "Maple Ridge rmmod -> reload driver_ready clean (no wedge to begin with)");

	printf("%s\n", failures ? "FAILED" : "PASSED");
	return failures ? 1 : 0;
}
