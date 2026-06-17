// SPDX-License-Identifier: GPL-2.0
/*
 * Userspace mirror of the ICM warm/cold reset-behaviour KUnit (test.c:
 * tb_test_icm_warm_restart_reauth). Firmware reverse-engineering of the Titan
 * Ridge ICM (AppMana/intel-thunderbolt-firmwares, out/ICM_8051_FINDINGS.md)
 * established that the NVM authentication asserting REG_FW_STS_NVM_AUTH_DONE is
 * performed ONLY by the on-die mask ROM at a true chip reset; it is absent from
 * the flashed 8051/ARC application image (no NVM/SPI read, no hash/crypto, no
 * writer of that bit in any of the 5 8051 banks or 6 ARC700 blobs).
 *
 * The kernel's icm_firmware_reset() (ICM_EN_CPU) is a warm CPU restart that
 * re-enters the application image, which reads READ-ONLY reset-cause registers
 * (CA41/CB5E), sees "warm", and skips re-init -- so on Alpine/Titan Ridge it
 * NEVER re-authenticates. Maple Ridge has a real reset vector and re-auths on a
 * warm restart. Hence a live rmmod+modprobe on AR/TR is terminal until a board
 * power cycle, and icm_stop() must NOT attempt a runtime reset (an earlier
 * reset-on-unload "fix" was hardware-disproven and reverted).
 *
 * ar_tr is the genuine controller property, not a toggle: the outcome follows
 * from the modelled firmware. Keep in lockstep with the KUnit in test.c.
 *
 *   cc -o /tmp/icm icm_reset_userspace.c && /tmp/icm
 */
#include <stdbool.h>
#include <stdio.h>

static int failures;
#define CHECK(c, m) do { \
	printf("  [%s] %s\n", (c) ? "ok" : "FAIL", m); \
	if (!(c)) failures++; \
} while (0)

struct icm_fw { bool icm_en; bool authed; };

/* Cold boot / board power cycle: the mask ROM runs, authenticates, hands off. */
static void icm_fw_cold_boot(struct icm_fw *fw)
{
	fw->icm_en = true;
	fw->authed = true;
}

/*
 * Warm CPU restart (icm_firmware_reset / ICM_EN_CPU). The mask ROM is NOT
 * re-entered. AR/TR: the application's read-only reset-cause gate skips re-init
 * -> stays de-authenticated until a cold boot. Maple: real reset vector
 * re-runs init -> re-authenticates.
 */
static void icm_fw_warm_restart(struct icm_fw *fw, bool ar_tr)
{
	fw->icm_en = true;
	fw->authed = !ar_tr;
}

static bool icm_fw_driver_ready(struct icm_fw *fw)
{
	return fw->icm_en && fw->authed;
}

int main(void)
{
	struct icm_fw fw;

	printf("icm warm/cold reset re-auth (mask-ROM auth):\n");

	icm_fw_cold_boot(&fw);
	CHECK(icm_fw_driver_ready(&fw), "cold boot -> authenticated -> driver_ready");

	/* AR/TR live reload (warm restart): never re-auths -> terminal. */
	icm_fw_cold_boot(&fw);
	icm_fw_warm_restart(&fw, /*ar_tr=*/true);
	CHECK(!icm_fw_driver_ready(&fw),
	      "Titan/Alpine Ridge live reload -> NOT authenticated (terminal until power cycle)");

	/* Only a cold boot recovers AR/TR. */
	icm_fw_cold_boot(&fw);
	CHECK(icm_fw_driver_ready(&fw), "cold boot (reboot/power cycle) re-authenticates AR/TR");

	/* Maple Ridge re-auths on a warm restart (real reset vector). */
	icm_fw_cold_boot(&fw);
	icm_fw_warm_restart(&fw, /*ar_tr=*/false);
	CHECK(icm_fw_driver_ready(&fw),
	      "Maple Ridge live reload -> re-authenticates (unaffected)");

	printf("%s\n", failures ? "FAILED" : "PASSED");
	return failures ? 1 : 0;
}
