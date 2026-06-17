/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ICM unload-reset policy (shared between icm.c and its tests).
 *
 * On Alpine/Titan Ridge the integrated connection manager firmware survives a
 * driver rmmod: icm_stop() sends NHI_MAILBOX_DRV_UNLOADS but the firmware
 * stays "running" (REG_FW_STS_ICM_EN set) in a state where it no longer
 * answers the ICM_DRIVER_READY handshake. The next driver load therefore times
 * out -- "failed to send driver ready to ICM" / probe error -110 -- and only a
 * host cold boot recovers it. To make an rmmod+modprobe cycle clean we issue a
 * CIO/firmware reset on the unload path so the ICM comes back freshly
 * initialized, exactly as it would after a cold boot.
 *
 * This header isolates the pure decision of *whether* to do that reset so it
 * can be unit-tested without an NHI/PCIe device. The includer provides bool
 * (kernel: linux/types.h; userspace test: stdbool.h).
 */
#ifndef ICM_RESET_H
#define ICM_RESET_H

/*
 * Should icm_stop() reset the ICM firmware to a clean state on this unload?
 *
 *   have_cio_reset     - controller exposes a CIO reset op (Alpine/Titan Ridge
 *                        family). USB4/Maple Ridge has none and does not wedge.
 *   have_upstream_port - the vendor-capability PCIe port used to drive the
 *                        reset is cached; without it the reset cannot be issued.
 *   going_away         - the controller is physically gone (NHI going away);
 *                        skip, since the config/mailbox writes would fault and
 *                        there is nothing to re-initialize.
 */
static inline bool icm_stop_should_reset_firmware(bool have_cio_reset,
						  bool have_upstream_port,
						  bool going_away)
{
	return have_cio_reset && have_upstream_port && !going_away;
}

#endif /* ICM_RESET_H */
