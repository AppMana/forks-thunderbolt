# SPDX-License-Identifier: GPL-2.0

# Shadow the kernel's <linux/thunderbolt.h> with the fork-patched copy so tbnet
# sees the same struct layout as the vendored core it is built against. See
# drivers/thunderbolt/Makefile for the rationale; empty in-tree / under KUnit.
ifneq ($(TBV_THUNDERBOLT_H_DIR),)
LINUXINCLUDE := -I$(TBV_THUNDERBOLT_H_DIR) $(LINUXINCLUDE)
endif

# Package version from dkms.conf, passed down by the dkms Makefile; absent
# in-tree / under KUnit, where MODULE_VERSION compiles out.
ifneq ($(TBV_PKG_VERSION),)
ccflags-y += -DTBV_PKG_VERSION=\"$(TBV_PKG_VERSION)\"
endif

obj-$(CONFIG_USB4_NET) := thunderbolt_net.o
thunderbolt_net-objs := main.o trace.o

# Login-handshake KUnit tests, built into the module only on a CONFIG_KUNIT
# kernel. Run via drivers/thunderbolt_ibverbs/tools/run-kunit.sh (kunit.py).
thunderbolt_net-$(CONFIG_USB4_NET_KUNIT_TEST) += tests/tbnet_login_test.o

# Tracepoints need to know where to find trace.h
CFLAGS_trace.o := -I$(src)
