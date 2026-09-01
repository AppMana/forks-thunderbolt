/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Thunderbolt DMA configuration based mailbox support
 *
 * Copyright (C) 2017, Intel Corporation
 * Authors: Michael Jamet <michael.jamet@intel.com>
 *          Mika Westerberg <mika.westerberg@linux.intel.com>
 */

#ifndef DMA_PORT_H_
#define DMA_PORT_H_

#include "tb.h"

struct tb_switch;
struct tb_dma_port;
struct tb_ctl;
struct tb_cfg_result;

#define DMA_PORT_CSS_ADDRESS		0x3fffff
#define DMA_PORT_CSS_MAX_SIZE		SZ_128

#define DMA_PORT_CAP			0x3e
#define DMA_PORT_MAIL_IN		(DMA_PORT_CAP + 17)
#define DMA_PORT_POWER_CYCLE_REQUEST	0x40000001
#define DMA_PORT_POWER_CYCLE_RAW_TIMEOUT_MS	500

static inline u8 dma_port_config_sequence(void)
{
	return 1;
}

static inline int dma_port_for_nhi(u16 vendor, u16 device)
{
	if (vendor != PCI_VENDOR_ID_INTEL)
		return -ENODEV;

	switch (device) {
	case PCI_DEVICE_ID_INTEL_MAPLE_RIDGE_2C_NHI:
	case PCI_DEVICE_ID_INTEL_MAPLE_RIDGE_4C_NHI:
		return 0xb;
	default:
		return -ENODEV;
	}
}

struct tb_dma_port *dma_port_alloc(struct tb_switch *sw);
void dma_port_free(struct tb_dma_port *dma);
int dma_port_flash_read(struct tb_dma_port *dma, unsigned int address,
			void *buf, size_t size);
int dma_port_flash_update_auth(struct tb_dma_port *dma);
int dma_port_flash_update_auth_status(struct tb_dma_port *dma, u32 *status);
int dma_port_flash_write(struct tb_dma_port *dma, unsigned int address,
			 const void *buf, size_t size);
int dma_port_power_cycle(struct tb_dma_port *dma);
struct tb_cfg_result dma_port_power_cycle_raw(struct tb_ctl *ctl, u8 port);

#endif
