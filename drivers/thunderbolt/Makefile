# SPDX-License-Identifier: GPL-2.0-only

# The fork replaces the whole thunderbolt subsystem and owns its public header:
# the vendored .c reference struct members the stock <linux/thunderbolt.h>
# lacks (tb_protocol_handler::callback_xd). The tbfix
# build generates a patched copy from the kernel's own header and passes its
# directory here; prepending it to LINUXINCLUDE shadows the kernel's copy for
# a <linux/...> include (verified: LINUXINCLUDE is searched before the stock
# kernel include dirs). Self-contained -- no node header mutation. Empty when
# built in-tree / via the KUnit overlay, where the header is patched directly.
ifneq ($(TBV_THUNDERBOLT_H_DIR),)
LINUXINCLUDE := -I$(TBV_THUNDERBOLT_H_DIR) $(LINUXINCLUDE)
endif

ccflags-y := -I$(src)

# Package version from dkms.conf, passed down by the dkms Makefile; absent
# in-tree / under KUnit, where MODULE_VERSION compiles out.
ifneq ($(TBV_PKG_VERSION),)
ccflags-y += -DTBV_PKG_VERSION=\"$(TBV_PKG_VERSION)\"
endif

# Experiment builds only (TBV_DEBUGFS_WRITE=1 in the environment): compile in
# the upstream debugfs register-write support so live register experiments can
# run without bespoke driver code. Never set for released packages.
ifneq ($(TBV_DEBUGFS_WRITE),)
ccflags-y += -DCONFIG_USB4_DEBUGFS_WRITE=1
endif

obj-${CONFIG_USB4} := thunderbolt.o
thunderbolt-objs := nhi.o nhi_ops.o ctl.o tb.o switch.o cap.o path.o tunnel.o eeprom.o
thunderbolt-objs += domain.o dma_port.o icm.o property.o xdomain.o lc.o tmu.o usb4.o
thunderbolt-objs += usb4_port.o nvm.o retimer.o quirks.o clx.o

thunderbolt-${CONFIG_ACPI} += acpi.o
thunderbolt-$(CONFIG_DEBUG_FS) += debugfs.o
thunderbolt-${CONFIG_USB4_KUNIT_TEST} += test.o
CFLAGS_test.o += $(DISABLE_STRUCTLEAK_PLUGIN)

thunderbolt_dma_test-${CONFIG_USB4_DMA_TEST} += dma_test.o
obj-$(CONFIG_USB4_DMA_TEST) += thunderbolt_dma_test.o
