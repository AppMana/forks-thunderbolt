/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Thunderbolt driver - NHI driver
 *
 * Copyright (c) 2014 Andreas Noever <andreas.noever@gmail.com>
 * Copyright (C) 2018, Intel Corporation
 */

#ifndef DSL3510_H_
#define DSL3510_H_

#include <linux/thunderbolt.h>

enum nhi_fw_mode {
	NHI_FW_SAFE_MODE,
	NHI_FW_AUTH_MODE,
	NHI_FW_EP_MODE,
	NHI_FW_CM_MODE,
};

enum nhi_mailbox_cmd {
	NHI_MAILBOX_SAVE_DEVS = 0x05,
	NHI_MAILBOX_DISCONNECT_PCIE_PATHS = 0x06,
	NHI_MAILBOX_DRV_UNLOADS = 0x07,
	NHI_MAILBOX_DISCONNECT_PA = 0x10,
	NHI_MAILBOX_DISCONNECT_PB = 0x11,
	NHI_MAILBOX_ALLOW_ALL_DEVS = 0x23,
};

int nhi_mailbox_cmd(struct tb_nhi *nhi, enum nhi_mailbox_cmd cmd, u32 data);
enum nhi_fw_mode nhi_mailbox_mode(struct tb_nhi *nhi);

/**
 * struct tb_nhi_ops - NHI specific optional operations
 * @init: NHI specific initialization
 * @suspend_noirq: NHI specific suspend_noirq hook
 * @resume_noirq: NHI specific resume_noirq hook
 * @runtime_suspend: NHI specific runtime_suspend hook
 * @runtime_resume: NHI specific runtime_resume hook
 * @shutdown: NHI specific shutdown
 */
struct tb_nhi_ops {
	int (*init)(struct tb_nhi *nhi);
	int (*suspend_noirq)(struct tb_nhi *nhi, bool wakeup);
	int (*resume_noirq)(struct tb_nhi *nhi);
	int (*runtime_suspend)(struct tb_nhi *nhi);
	int (*runtime_resume)(struct tb_nhi *nhi);
	void (*shutdown)(struct tb_nhi *nhi);
};

extern const struct tb_nhi_ops icl_nhi_ops;

/*
 * PCI IDs used in this driver from Win Ridge forward. There is no
 * need for the PCI quirk anymore as we will use ICM also on Apple
 * hardware.
 */
#define PCI_DEVICE_ID_INTEL_MAPLE_RIDGE_2C_NHI		0x1134
/*
 * Both the 2C and the 4C Maple Ridge parts present their PCIe upstream
 * bridge as 0x1136; get_upstream_port() needs it to reach the CIO reset
 * VSEC, which is the precondition for icm_firmware_reset() on this silicon.
 */
#define PCI_DEVICE_ID_INTEL_MAPLE_RIDGE_BRIDGE		0x1136
#define PCI_DEVICE_ID_INTEL_MAPLE_RIDGE_4C_NHI		0x1137
#define PCI_DEVICE_ID_INTEL_WIN_RIDGE_2C_NHI            0x157d
#define PCI_DEVICE_ID_INTEL_WIN_RIDGE_2C_BRIDGE         0x157e
#define PCI_DEVICE_ID_INTEL_ALPINE_RIDGE_LP_NHI		0x15bf
#define PCI_DEVICE_ID_INTEL_ALPINE_RIDGE_LP_BRIDGE	0x15c0
#define PCI_DEVICE_ID_INTEL_ALPINE_RIDGE_C_4C_NHI	0x15d2
#define PCI_DEVICE_ID_INTEL_ALPINE_RIDGE_C_4C_BRIDGE	0x15d3
#define PCI_DEVICE_ID_INTEL_ALPINE_RIDGE_C_2C_NHI	0x15d9
#define PCI_DEVICE_ID_INTEL_ALPINE_RIDGE_C_2C_BRIDGE	0x15da
#define PCI_DEVICE_ID_INTEL_ALPINE_RIDGE_LP_USBONLY_NHI	0x15dc
#define PCI_DEVICE_ID_INTEL_ALPINE_RIDGE_USBONLY_NHI	0x15dd
#define PCI_DEVICE_ID_INTEL_ALPINE_RIDGE_C_USBONLY_NHI	0x15de
#define PCI_DEVICE_ID_INTEL_TITAN_RIDGE_2C_BRIDGE	0x15e7
#define PCI_DEVICE_ID_INTEL_TITAN_RIDGE_2C_NHI		0x15e8
#define PCI_DEVICE_ID_INTEL_TITAN_RIDGE_4C_BRIDGE	0x15ea
#define PCI_DEVICE_ID_INTEL_TITAN_RIDGE_4C_NHI		0x15eb
#define PCI_DEVICE_ID_INTEL_TITAN_RIDGE_DD_BRIDGE	0x15ef
#define PCI_DEVICE_ID_INTEL_ADL_NHI0			0x463e
#define PCI_DEVICE_ID_INTEL_ADL_NHI1			0x466d
#define PCI_DEVICE_ID_INTEL_WCL_NHI0			0x4d33
#define PCI_DEVICE_ID_INTEL_BARLOW_RIDGE_HOST_80G_NHI	0x5781
#define PCI_DEVICE_ID_INTEL_BARLOW_RIDGE_HOST_40G_NHI	0x5784
#define PCI_DEVICE_ID_INTEL_BARLOW_RIDGE_HUB_80G_BRIDGE 0x5786
#define PCI_DEVICE_ID_INTEL_BARLOW_RIDGE_HUB_40G_BRIDGE 0x57a4
#define PCI_DEVICE_ID_INTEL_MTL_M_NHI0			0x7eb2
#define PCI_DEVICE_ID_INTEL_MTL_P_NHI0			0x7ec2
#define PCI_DEVICE_ID_INTEL_MTL_P_NHI1			0x7ec3
#define PCI_DEVICE_ID_INTEL_ICL_NHI1			0x8a0d
#define PCI_DEVICE_ID_INTEL_ICL_NHI0			0x8a17
#define PCI_DEVICE_ID_INTEL_TGL_NHI0			0x9a1b
#define PCI_DEVICE_ID_INTEL_TGL_NHI1			0x9a1d
#define PCI_DEVICE_ID_INTEL_TGL_H_NHI0			0x9a1f
#define PCI_DEVICE_ID_INTEL_TGL_H_NHI1			0x9a21
#define PCI_DEVICE_ID_INTEL_RPL_NHI0			0xa73e
#define PCI_DEVICE_ID_INTEL_RPL_NHI1			0xa76d
#define PCI_DEVICE_ID_INTEL_LNL_NHI0			0xa833
#define PCI_DEVICE_ID_INTEL_LNL_NHI1			0xa834
#define PCI_DEVICE_ID_INTEL_PTL_M_NHI0			0xe333
#define PCI_DEVICE_ID_INTEL_PTL_M_NHI1			0xe334
#define PCI_DEVICE_ID_INTEL_PTL_P_NHI0			0xe433
#define PCI_DEVICE_ID_INTEL_PTL_P_NHI1			0xe434

#define PCI_CLASS_SERIAL_USB_USB4			0x0c0340

/*
 * tb_icm_warm_restart_supported() - may a stuck ICM be restarted in software?
 * @nhi_device_id: PCI device id of the NHI
 *
 * The blanket "never icm_firmware_reset() an ICM that advertises running" rule
 * came from Alpine/Titan Ridge, where NVM_AUTH_DONE is asserted only by the
 * on-die mask ROM at a true chip reset: a warm ICM_EN_CPU restart there comes
 * back UNAUTHENTICATED, which is terminal, and is why tbfix 1.7/1.8 were
 * reverted.
 *
 * That is silicon-specific and does NOT hold for Maple Ridge. Our own
 * decompiles (AppMana/intel-thunderbolt-firmwares, out/ICM_PROTOCOL.md §6)
 * found mr_bank0.c carries ZERO references to the 0xCA41/0xCB5E warm/cold
 * reset-cause registers, against 310 in Titan's tr_bank0.c -- "Maple's
 * resident image has a real reset vector and no warm gate, consistent with it
 * surviving a live rmmod/modprobe reset where Titan wedges."
 *
 * Refusing to try therefore costs a Maple Ridge host until someone physically
 * pulls its power. Measured on appmana-019 (8086:1137, Maple Ridge 4C) on
 * 2026-08-25: ring 0 alive in BOTH directions (tx_done 938, tx_canceled 0,
 * rx_total 970, rx_dropped 0) while the firmware endlessly re-emitted a single
 * undefined ICM_RESP with code 0x08 and answered no request. Executing, but
 * stuck -- exactly what a restart is for. A healthy sibling on the same image
 * (appmana-027) showed zero unmatched replies and zero timeouts.
 *
 * Pure predicate, exercised by KUnit.
 */
static inline bool tb_icm_warm_restart_supported(u16 nhi_device_id)
{
	switch (nhi_device_id) {
	case PCI_DEVICE_ID_INTEL_MAPLE_RIDGE_2C_NHI:
	case PCI_DEVICE_ID_INTEL_MAPLE_RIDGE_4C_NHI:
		return true;
	default:
		return false;
	}
}

#endif
