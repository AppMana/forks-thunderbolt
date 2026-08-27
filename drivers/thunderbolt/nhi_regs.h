/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Thunderbolt driver - NHI registers
 *
 * Copyright (c) 2014 Andreas Noever <andreas.noever@gmail.com>
 * Copyright (C) 2018, Intel Corporation
 */

#ifndef NHI_REGS_H_
#define NHI_REGS_H_

#include <linux/errno.h>
#include <linux/types.h>

enum ring_flags {
	RING_FLAG_ISOCH_ENABLE = 1 << 27, /* TX only? */
	RING_FLAG_E2E_FLOW_CONTROL = 1 << 28,
	RING_FLAG_PCI_NO_SNOOP = 1 << 29,
	RING_FLAG_RAW = 1 << 30, /* ignore EOF/SOF mask, include checksum */
	RING_FLAG_ENABLE = 1 << 31,
};

/**
 * struct ring_desc - TX/RX ring entry
 *
 * For TX set length/eof/sof.
 * For RX length/eof/sof are set by the NHI.
 */
struct ring_desc {
	u64 phys;
	union {
		struct {
			u32 length:12;
			u32 eof:4;
			u32 sof:4;
			enum ring_desc_flags flags:12;
		};
		u32 attributes;
	};
	u32 time; /* write zero */
} __packed;

/* NHI registers in bar 0 */

/*
 * 16 bytes per entry, one entry for every hop (REG_CAPS)
 * 00: physical pointer to an array of struct ring_desc
 * 08: ring tail (set by NHI)
 * 10: ring head (index of first non posted descriptor)
 * 12: descriptor count
 */
#define REG_TX_RING_BASE	0x00000

/*
 * 16 bytes per entry, one entry for every hop (REG_CAPS)
 * 00: physical pointer to an array of struct ring_desc
 * 08: ring head (index of first not posted descriptor)
 * 10: ring tail (set by NHI)
 * 12: descriptor count
 * 14: max frame sizes (anything larger than 0x100 has no effect)
 */
#define REG_RX_RING_BASE	0x08000

/*
 * 32 bytes per entry, one entry for every hop (REG_CAPS)
 * 00: enum_ring_flags
 * 04: isoch time stamp ?? (write 0)
 * ..: unknown
 */
#define REG_TX_OPTIONS_BASE	0x19800

/*
 * 32 bytes per entry, one entry for every hop (REG_CAPS)
 * 00: enum ring_flags
 *     If RING_FLAG_E2E_FLOW_CONTROL is set then bits 13-23 must be set to
 *     the corresponding TX hop id.
 * 04: EOF/SOF mask (ignored for RING_FLAG_RAW rings)
 * ..: unknown
 */
#define REG_RX_OPTIONS_BASE	0x29800
#define REG_RX_OPTIONS_E2E_HOP_MASK	GENMASK(22, 12)
#define REG_RX_OPTIONS_E2E_HOP_SHIFT	12

/*
 * three bitfields: tx, rx, rx overflow
 * Every bitfield contains one bit for every hop (REG_CAPS).
 * New interrupts are fired only after ALL registers have been
 * read (even those containing only disabled rings).
 */
#define REG_RING_NOTIFY_BASE	0x37800
#define RING_NOTIFY_REG_COUNT(nhi) ((31 + 3 * nhi->hop_count) / 32)
#define REG_RING_INT_CLEAR	0x37808

/*
 * two bitfields: rx, tx
 * Both bitfields contains one bit for every hop (REG_CAPS). To
 * enable/disable interrupts set/clear the corresponding bits.
 */
#define REG_RING_INTERRUPT_BASE	0x38200
#define RING_INTERRUPT_REG_COUNT(nhi) ((31 + 2 * nhi->hop_count) / 32)

#define REG_RING_INTERRUPT_MASK_CLEAR_BASE	0x38208

#define REG_INT_THROTTLING_RATE			0x38c00
#define REG_INT_THROTTLING_RATE_INTERVAL_MASK	GENMASK(15, 0)

/* Interrupt Vector Allocation */
#define REG_INT_VEC_ALLOC_BASE	0x38c40
#define REG_INT_VEC_ALLOC_BITS	4
#define REG_INT_VEC_ALLOC_MASK	GENMASK(3, 0)
#define REG_INT_VEC_ALLOC_REGS	(32 / REG_INT_VEC_ALLOC_BITS)

/* The last 11 bits contain the number of hops supported by the NHI port. */
#define REG_CAPS			0x39640
#define REG_CAPS_VERSION_MASK		GENMASK(23, 16)
#define REG_CAPS_VERSION_2		0x40

#define REG_DMA_MISC			0x39864
#define REG_DMA_MISC_INT_AUTO_CLEAR     BIT(2)
#define REG_DMA_MISC_DISABLE_AUTO_CLEAR	BIT(17)

enum tb_nhi_irq_mode {
	TB_NHI_IRQ_AUTO_STATUS_CLEAR,
	TB_NHI_IRQ_CLEAR_ON_READ,
	TB_NHI_IRQ_EXPLICIT_W1C,
	TB_NHI_IRQ_INVALID,
};

static inline enum tb_nhi_irq_mode tb_nhi_dma_misc_interrupt_mode(u32 value)
{
	bool auto_clear = value & REG_DMA_MISC_INT_AUTO_CLEAR;
	bool disable_clear_on_read = value & REG_DMA_MISC_DISABLE_AUTO_CLEAR;

	if (auto_clear && disable_clear_on_read)
		return TB_NHI_IRQ_INVALID;
	if (auto_clear)
		return TB_NHI_IRQ_AUTO_STATUS_CLEAR;
	if (disable_clear_on_read)
		return TB_NHI_IRQ_EXPLICIT_W1C;

	return TB_NHI_IRQ_CLEAR_ON_READ;
}

static inline u32
tb_nhi_dma_misc_interrupt_policy(u32 value, enum tb_nhi_irq_mode mode)
{
	value &= ~(REG_DMA_MISC_INT_AUTO_CLEAR |
		   REG_DMA_MISC_DISABLE_AUTO_CLEAR);
	if (mode == TB_NHI_IRQ_AUTO_STATUS_CLEAR)
		return value | REG_DMA_MISC_INT_AUTO_CLEAR;
	if (mode == TB_NHI_IRQ_EXPLICIT_W1C)
		return value | REG_DMA_MISC_DISABLE_AUTO_CLEAR;

	return value;
}

enum tb_nhi_irq_setup_phase {
	TB_NHI_IRQ_SETUP_RESET,
	TB_NHI_IRQ_SETUP_MASKED,
	TB_NHI_IRQ_SETUP_STATUS_DRAINED,
	TB_NHI_IRQ_SETUP_MODE_SET,
	TB_NHI_IRQ_SETUP_READY,
	TB_NHI_IRQ_SETUP_INVALID,
};

enum tb_nhi_irq_setup_event {
	TB_NHI_IRQ_SETUP_MASK_ALL,
	TB_NHI_IRQ_SETUP_DRAIN_STATUS,
	TB_NHI_IRQ_SETUP_SET_MODE,
	TB_NHI_IRQ_SETUP_FLUSH_MODE,
};

static inline enum tb_nhi_irq_setup_phase
tb_nhi_irq_setup_next(enum tb_nhi_irq_setup_phase phase,
		      enum tb_nhi_irq_setup_event event)
{
	switch (phase) {
	case TB_NHI_IRQ_SETUP_RESET:
		if (event == TB_NHI_IRQ_SETUP_MASK_ALL)
			return TB_NHI_IRQ_SETUP_MASKED;
		break;
	case TB_NHI_IRQ_SETUP_MASKED:
		if (event == TB_NHI_IRQ_SETUP_DRAIN_STATUS)
			return TB_NHI_IRQ_SETUP_STATUS_DRAINED;
		break;
	case TB_NHI_IRQ_SETUP_STATUS_DRAINED:
		if (event == TB_NHI_IRQ_SETUP_SET_MODE)
			return TB_NHI_IRQ_SETUP_MODE_SET;
		break;
	case TB_NHI_IRQ_SETUP_MODE_SET:
		if (event == TB_NHI_IRQ_SETUP_FLUSH_MODE)
			return TB_NHI_IRQ_SETUP_READY;
		break;
	case TB_NHI_IRQ_SETUP_READY:
	case TB_NHI_IRQ_SETUP_INVALID:
		break;
	}

	return TB_NHI_IRQ_SETUP_INVALID;
}

#define REG_RESET			0x39898
#define REG_RESET_HRR			BIT(0)

#define REG_INMAIL_DATA			0x39900

#define REG_INMAIL_CMD			0x39904
#define REG_INMAIL_CMD_MASK		GENMASK(7, 0)
#define REG_INMAIL_ERROR		BIT(30)
#define REG_INMAIL_OP_REQUEST		BIT(31)

#define REG_OUTMAIL_CMD			0x3990c
#define REG_OUTMAIL_CMD_OPMODE_SHIFT	8
#define REG_OUTMAIL_CMD_OPMODE_MASK	GENMASK(11, 8)

#define REG_FW_STS			0x39944
#define REG_FW_STS_NVM_AUTH_DONE	BIT(31)
#define REG_FW_STS_CIO_RESET_REQ	BIT(30)
#define REG_FW_STS_ICM_EN_CPU		BIT(2)
#define REG_FW_STS_ICM_EN_INVERT	BIT(1)
#define REG_FW_STS_ICM_EN		BIT(0)

#define PCIE2CIO_CMD			0x30
#define PCIE2CIO_CMD_TIMEOUT		BIT(31)
#define PCIE2CIO_CMD_START		BIT(30)
#define PCIE2CIO_CMD_WRITE		BIT(21)
#define PCIE2CIO_CMD_CS_MASK		GENMASK(20, 19)
#define PCIE2CIO_CMD_CS_SHIFT		19
#define PCIE2CIO_CMD_PORT_MASK		GENMASK(18, 13)
#define PCIE2CIO_CMD_PORT_SHIFT		13

enum tb_pcie2cio_completion_state {
	TB_PCIE2CIO_COMPLETION_BUSY,
	TB_PCIE2CIO_COMPLETION_COMPLETE,
	TB_PCIE2CIO_COMPLETION_TARGET_TIMEOUT,
};

static inline enum tb_pcie2cio_completion_state
tb_pcie2cio_completion_state(u32 cmd)
{
	if (cmd & PCIE2CIO_CMD_START)
		return TB_PCIE2CIO_COMPLETION_BUSY;
	if (cmd & PCIE2CIO_CMD_TIMEOUT)
		return TB_PCIE2CIO_COMPLETION_TARGET_TIMEOUT;

	return TB_PCIE2CIO_COMPLETION_COMPLETE;
}

/*
 * Is the ICM firmware running, judged from a raw REG_FW_STS read? Single
 * source for icm_firmware_running() and the KUnit model.
 *
 * A dead or hung NHI reads all-ones from MMIO, which spuriously asserts
 * ICM_EN (and every other bit). Treating that as "firmware running" routes a
 * wedged controller into the firmware connection manager, where DRIVER_READY
 * can only time out and the whole domain is lost with a misleading "failed to
 * send driver ready to ICM". Treat ~0 as "no firmware".
 */
static inline bool tb_icm_fw_sts_running(u32 fw_sts)
{
	return fw_sts != (u32)~0U && (fw_sts & REG_FW_STS_ICM_EN);
}

/*
 * Is a firmware-CM probe failure the wedged-ICM signature? DRIVER_READY
 * failed (@driver_ready_err) while REG_FW_STS still advertises ICM_EN:
 * the status bit is latched but the command that failed is not a complete
 * liveness probe. This is a diagnosis, not permission to replace the
 * firmware connection manager: a partial failure may still own other ring-0
 * traffic. Single source for icm_driver_ready() and the KUnit model
 * (tb_test_icm_partial_wedge_refuses_software_takeover).
 */
static inline bool tb_icm_wedged(int driver_ready_err, u32 fw_sts)
{
	return driver_ready_err && tb_icm_fw_sts_running(fw_sts);
}

enum tb_icm_startup_proof_event {
	TB_ICM_PROOF_DRIVER_READY,
	TB_ICM_PROOF_ROOT_CONFIG,
};

struct tb_icm_startup_proof {
	bool firmware_ready;
	bool mailbox_ready;
	bool driver_ready;
	bool root_config_ready;
};

/*
 * Keep the controller's status claims separate from end-to-end control-path
 * proofs. A set firmware-ready bit or the expected mailbox mode says nothing
 * about whether DriverReady completes or router config traffic is live.
 */
static inline struct tb_icm_startup_proof
tb_icm_startup_proof_begin(u32 fw_sts, bool mailbox_ready)
{
	return (struct tb_icm_startup_proof) {
		.firmware_ready = fw_sts != (u32)~0U &&
				  (fw_sts & REG_FW_STS_NVM_AUTH_DONE),
		.mailbox_ready = mailbox_ready,
	};
}

static inline void
tb_icm_startup_proof_advance(struct tb_icm_startup_proof *proof,
			     enum tb_icm_startup_proof_event event)
{
	switch (event) {
	case TB_ICM_PROOF_DRIVER_READY:
		proof->driver_ready = true;
		break;
	case TB_ICM_PROOF_ROOT_CONFIG:
		if (proof->driver_ready)
			proof->root_config_ready = true;
		break;
	}
}

#define TB_ICM_ROOT_CONFIG_TIMEOUT_MS	100U
#define TB_ICM_ROOT_CONFIG_INTERVAL_MS	50U

static inline unsigned int
tb_icm_root_config_request_count(unsigned int passes)
{
	return passes;
}

static inline unsigned int
tb_icm_root_config_request_budget(unsigned int passes)
{
	return tb_icm_root_config_request_count(passes);
}

static inline u64 tb_icm_root_config_budget_ms(unsigned int passes)
{
	return (u64)tb_icm_root_config_request_count(passes) *
		(TB_ICM_ROOT_CONFIG_TIMEOUT_MS +
		 TB_ICM_ROOT_CONFIG_INTERVAL_MS);
}

enum tb_icm_root_recovery_state {
	TB_ICM_ROOT_RECOVERY_IDLE,
	TB_ICM_ROOT_RECOVERY_POWER_CYCLE_PENDING,
	TB_ICM_ROOT_RECOVERY_REPROBE_REQUIRED,
	TB_ICM_ROOT_RECOVERY_TERMINAL,
};

enum tb_icm_root_recovery_event {
	TB_ICM_ROOT_RECOVERY_ROOT_CONFIG_TIMEOUT,
	TB_ICM_ROOT_RECOVERY_OTHER_FAILURE,
	TB_ICM_ROOT_RECOVERY_POWER_CYCLE_DISPATCHED,
	TB_ICM_ROOT_RECOVERY_POWER_CYCLE_FAILED,
};

static inline enum tb_icm_root_recovery_event
tb_icm_root_recovery_command_event(int err, bool tx_consumed)
{
	if (!err || (err == -ETIMEDOUT && tx_consumed))
		return TB_ICM_ROOT_RECOVERY_POWER_CYCLE_DISPATCHED;

	return TB_ICM_ROOT_RECOVERY_POWER_CYCLE_FAILED;
}

static inline enum tb_icm_root_recovery_state
tb_icm_root_recovery_next(enum tb_icm_root_recovery_state state,
			  enum tb_icm_root_recovery_event event)
{
	switch (state) {
	case TB_ICM_ROOT_RECOVERY_IDLE:
		if (event == TB_ICM_ROOT_RECOVERY_ROOT_CONFIG_TIMEOUT)
			return TB_ICM_ROOT_RECOVERY_POWER_CYCLE_PENDING;
		return TB_ICM_ROOT_RECOVERY_TERMINAL;
	case TB_ICM_ROOT_RECOVERY_POWER_CYCLE_PENDING:
		if (event == TB_ICM_ROOT_RECOVERY_POWER_CYCLE_DISPATCHED)
			return TB_ICM_ROOT_RECOVERY_REPROBE_REQUIRED;
		return TB_ICM_ROOT_RECOVERY_TERMINAL;
	case TB_ICM_ROOT_RECOVERY_REPROBE_REQUIRED:
	case TB_ICM_ROOT_RECOVERY_TERMINAL:
		return state;
	}

	return TB_ICM_ROOT_RECOVERY_TERMINAL;
}

enum tb_nhi_recovery_state {
	TB_NHI_RECOVERY_IDLE,
	TB_NHI_RECOVERY_REPROBE_PENDING,
	TB_NHI_RECOVERY_RETRYING,
	TB_NHI_RECOVERY_COMPLETE,
	TB_NHI_RECOVERY_EXHAUSTED,
};

enum tb_nhi_recovery_event {
	TB_NHI_RECOVERY_DRIVER_READY_TIMEOUT,
	TB_NHI_RECOVERY_ROOT_CONFIG_TIMEOUT,
	TB_NHI_RECOVERY_OTHER_FAILURE,
	TB_NHI_RECOVERY_ROOT_POWER_CYCLE_DISPATCHED,
	TB_NHI_RECOVERY_ROOT_POWER_CYCLE_FAILED,
	TB_NHI_RECOVERY_REPROBE_DISPATCHED,
	TB_NHI_RECOVERY_REPROBE_QUEUE_FAILED,
	TB_NHI_RECOVERY_PROBE_SUCCEEDED,
};

enum tb_nhi_recovery_action {
	TB_NHI_RECOVERY_ACTION_NONE,
	TB_NHI_RECOVERY_ACTION_REPROBE,
};

static inline bool
tb_nhi_startup_recovery_may_dispatch(enum tb_nhi_recovery_state state)
{
	return state == TB_NHI_RECOVERY_IDLE;
}

static inline enum tb_nhi_recovery_action
tb_nhi_recovery_action(enum tb_nhi_recovery_state state)
{
	switch (state) {
	case TB_NHI_RECOVERY_REPROBE_PENDING:
		return TB_NHI_RECOVERY_ACTION_REPROBE;
	default:
		return TB_NHI_RECOVERY_ACTION_NONE;
	}
}

enum tb_nhi_runtime_recovery_state {
	TB_NHI_RUNTIME_RECOVERY_IDLE,
	TB_NHI_RUNTIME_RECOVERY_QUIESCE_PENDING,
	TB_NHI_RUNTIME_RECOVERY_ICM_RESET_PENDING,
	TB_NHI_RUNTIME_RECOVERY_REPROBE_PENDING,
	TB_NHI_RUNTIME_RECOVERY_VERIFYING,
	TB_NHI_RUNTIME_RECOVERY_COMPLETE,
	TB_NHI_RUNTIME_RECOVERY_POWER_REQUIRED,
};

enum tb_nhi_runtime_recovery_event {
	TB_NHI_RUNTIME_RECOVERY_DATA_TX_STALLED,
	TB_NHI_RUNTIME_RECOVERY_QUIESCE_SUCCEEDED,
	TB_NHI_RUNTIME_RECOVERY_QUIESCE_FAILED,
	TB_NHI_RUNTIME_RECOVERY_ICM_RESET_SUCCEEDED,
	TB_NHI_RUNTIME_RECOVERY_ICM_RESET_FAILED,
	TB_NHI_RUNTIME_RECOVERY_REPROBE_SUCCEEDED,
	TB_NHI_RUNTIME_RECOVERY_REPROBE_FAILED,
	TB_NHI_RUNTIME_RECOVERY_DATA_PATH_PROVEN,
};

static inline enum tb_nhi_runtime_recovery_state
tb_nhi_runtime_recovery_next(enum tb_nhi_runtime_recovery_state state,
			     enum tb_nhi_runtime_recovery_event event)
{
	switch (state) {
	case TB_NHI_RUNTIME_RECOVERY_IDLE:
		if (event == TB_NHI_RUNTIME_RECOVERY_DATA_TX_STALLED)
			return TB_NHI_RUNTIME_RECOVERY_QUIESCE_PENDING;
		return state;

	case TB_NHI_RUNTIME_RECOVERY_QUIESCE_PENDING:
		if (event == TB_NHI_RUNTIME_RECOVERY_QUIESCE_SUCCEEDED)
			return TB_NHI_RUNTIME_RECOVERY_ICM_RESET_PENDING;
		return TB_NHI_RUNTIME_RECOVERY_POWER_REQUIRED;

	case TB_NHI_RUNTIME_RECOVERY_ICM_RESET_PENDING:
		if (event == TB_NHI_RUNTIME_RECOVERY_ICM_RESET_SUCCEEDED)
			return TB_NHI_RUNTIME_RECOVERY_REPROBE_PENDING;
		return TB_NHI_RUNTIME_RECOVERY_POWER_REQUIRED;

	case TB_NHI_RUNTIME_RECOVERY_REPROBE_PENDING:
		if (event == TB_NHI_RUNTIME_RECOVERY_REPROBE_SUCCEEDED)
			return TB_NHI_RUNTIME_RECOVERY_VERIFYING;
		return TB_NHI_RUNTIME_RECOVERY_POWER_REQUIRED;

	case TB_NHI_RUNTIME_RECOVERY_VERIFYING:
		if (event == TB_NHI_RUNTIME_RECOVERY_DATA_PATH_PROVEN)
			return TB_NHI_RUNTIME_RECOVERY_COMPLETE;
		if (event == TB_NHI_RUNTIME_RECOVERY_DATA_TX_STALLED)
			return TB_NHI_RUNTIME_RECOVERY_POWER_REQUIRED;
		return state;

	case TB_NHI_RUNTIME_RECOVERY_COMPLETE:
	case TB_NHI_RUNTIME_RECOVERY_POWER_REQUIRED:
		return state;
	}

	return TB_NHI_RUNTIME_RECOVERY_POWER_REQUIRED;
}

/* Keep recovery policy independent from the ICM startup proof machine above. */
static inline enum tb_nhi_recovery_state
tb_nhi_recovery_next(enum tb_nhi_recovery_state state,
		     enum tb_nhi_recovery_event event)
{
	switch (state) {
	case TB_NHI_RECOVERY_IDLE:
		if (event == TB_NHI_RECOVERY_ROOT_POWER_CYCLE_DISPATCHED)
			return TB_NHI_RECOVERY_REPROBE_PENDING;
		if (event == TB_NHI_RECOVERY_PROBE_SUCCEEDED)
			return TB_NHI_RECOVERY_COMPLETE;
		return TB_NHI_RECOVERY_EXHAUSTED;

	case TB_NHI_RECOVERY_REPROBE_PENDING:
		if (event == TB_NHI_RECOVERY_REPROBE_DISPATCHED)
			return TB_NHI_RECOVERY_RETRYING;
		return TB_NHI_RECOVERY_EXHAUSTED;

	case TB_NHI_RECOVERY_RETRYING:
		if (event == TB_NHI_RECOVERY_PROBE_SUCCEEDED)
			return TB_NHI_RECOVERY_COMPLETE;
		return TB_NHI_RECOVERY_EXHAUSTED;

	case TB_NHI_RECOVERY_COMPLETE:
	case TB_NHI_RECOVERY_EXHAUSTED:
		return state;
	}

	return TB_NHI_RECOVERY_EXHAUSTED;
}


/*
 * Must nhi_select_cm() hand the domain straight to the software connection
 * manager, without trying the firmware one? Single source for
 * nhi_select_cm() and the KUnit model (tb_test_cm_select_forced_software).
 *
 * The forced case deliberately takes no firmware state: the operator forces
 * the software CM precisely when a resident ICM is broken (trains links but
 * never sends device-connected -- ASRock X570 Creator, Titan Ridge NVM 45.0)
 * and cannot be stopped without losing NVM authentication (see icm_stop()).
 * Selection must therefore not depend on anything the firmware advertises.
 */
static inline bool tb_nhi_use_software_cm(bool force_sw_cm, bool acpi_native)
{
	return force_sw_cm || acpi_native;
}

/* ICL NHI VSEC registers */

/* FW ready */
#define VS_CAP_9			0xc8
#define VS_CAP_9_FW_READY		BIT(31)
/* UUID */
#define VS_CAP_10			0xcc
#define VS_CAP_11			0xd0
/* LTR */
#define VS_CAP_15			0xe0
#define VS_CAP_16			0xe4
/* TBT2PCIe */
#define VS_CAP_18			0xec
#define VS_CAP_18_DONE			BIT(0)
/* PCIe2TBT */
#define VS_CAP_19			0xf0
#define VS_CAP_19_VALID			BIT(0)
#define VS_CAP_19_CMD_SHIFT		1
#define VS_CAP_19_CMD_MASK		GENMASK(7, 1)
/* Force power */
#define VS_CAP_22			0xfc
#define VS_CAP_22_FORCE_POWER		BIT(1)
#define VS_CAP_22_DMA_DELAY_MASK	GENMASK(31, 24)
#define VS_CAP_22_DMA_DELAY_SHIFT	24

/**
 * enum icl_lc_mailbox_cmd - ICL specific LC mailbox commands
 * @ICL_LC_GO2SX: Ask LC to enter Sx without wake
 * @ICL_LC_GO2SX_NO_WAKE: Ask LC to enter Sx with wake
 * @ICL_LC_PREPARE_FOR_RESET: Prepare LC for reset
 */
enum icl_lc_mailbox_cmd {
	ICL_LC_GO2SX = 0x02,
	ICL_LC_GO2SX_NO_WAKE = 0x03,
	ICL_LC_PREPARE_FOR_RESET = 0x21,
};

#endif
