# SPDX-License-Identifier: GPL-2.0
#
# Out-of-tree build for thunderbolt_frame.ko, the lossless frame service
# (formerly tbframe; the tbframe_* symbol namespace is unchanged).
# Same tbfix-core discipline as ../Makefile: tbframe references the PATCHED
# thunderbolt core's exports (tb_ring_*, tb_xdomain_*), so it must build
# against the tbfix Module.symvers or it loads with wrong/absent CRCs
# ("disagrees about version of symbol", err -22).

KVER ?= $(shell uname -r)
KDIR ?= /lib/modules/$(KVER)/build
PWD := $(shell pwd)
TBFIX_SYMVERS ?= $(shell ls -1 /var/lib/dkms/thunderbolt-tbfix/*/$(KVER)/*/module/Module.symvers 2>/dev/null | sort -V | tail -1)
ifeq ($(TBFIX_SYMVERS),)
TBFIX_SYMVERS := $(shell ls -1 /usr/src/thunderbolt-tbfix-*/Module.symvers 2>/dev/null | sort -V | tail -1)
endif
ifneq ($(TBFIX_SYMVERS),)
EXTRA_SYMBOLS := KBUILD_EXTRA_SYMBOLS=$(TBFIX_SYMVERS)
endif

# Shadow the kernel's <linux/thunderbolt.h> with the fork-patched copy
# (callback_xd, tb_xdomain_paths_active, tb_reannounce_property_dirs), same
# mechanism as ../Makefile. When the caller does not hand one in, GENERATE it
# here: a bare `make TBFIX_SYMVERS=...` used to build source-blind against the
# stock header while loading cleanly against the tbfix core (CRCs come from
# the symvers file) -- before the core appended ->callback_xd behind the stock
# members, that layout split-brain sent dispatch through this module's ->data
# pointer (appmana-025 NX-execute panic, kdump 202608031305).
TBF_MK_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
ifeq ($(TBV_THUNDERBOLT_H_DIR),)
TBFIX_HDR_GEN := $(firstword \
	$(wildcard $(TBF_MK_DIR)../../../dkms/tbfix-gen-thunderbolt-header.sh) \
	$(shell ls -1 /usr/src/thunderbolt-tbfix-*/tbfix-gen-thunderbolt-header.sh 2>/dev/null | sort -V | tail -1))
ifneq ($(TBFIX_HDR_GEN),)
TBV_THUNDERBOLT_H_DIR := $(TBF_MK_DIR).tbfix-gen-include
TBFIX_HDR_RC := $(shell $(TBFIX_HDR_GEN) "$(KDIR)" "$(TBV_THUNDERBOLT_H_DIR)" >&2; echo $$?)
ifneq ($(TBFIX_HDR_RC),0)
$(error tbfix header shim generation failed: $(TBFIX_HDR_GEN) "$(KDIR)")
endif
else
$(warning tbfix header shim script not found; building source-blind against the stock <linux/thunderbolt.h>)
endif
endif
ifneq ($(TBV_THUNDERBOLT_H_DIR),)
LINUXINCLUDE := -I$(TBV_THUNDERBOLT_H_DIR) $(LINUXINCLUDE)
endif

obj-m := thunderbolt_frame.o
thunderbolt_frame-y := main.o core.o service.o hw.o

# KUnit contract tests against the mock ring layer (tbframe_hw_ops). Run
# with drivers/thunderbolt_frame/tools/run-kunit.sh; host kernels lack
# CONFIG_KUNIT.
thunderbolt_frame-$(CONFIG_KUNIT) += tests/session_test.o
thunderbolt_frame-$(CONFIG_KUNIT) += tests/supersede_test.o
thunderbolt_frame-$(CONFIG_KUNIT) += tests/window_test.o
thunderbolt_frame-$(CONFIG_KUNIT) += tests/teardown_test.o
thunderbolt_frame-$(CONFIG_KUNIT) += tests/lifecycle_test.o
thunderbolt_frame-$(CONFIG_KUNIT) += tests/selfloop_test.o

ccflags-y := -Wall -Wextra -Wno-unused-parameter -I$(src)

# pr_debug sites stay compiled in and runtime-enableable through dynamic debug
# (DYNAMIC_DEBUG_MODULE covers CONFIG_DYNAMIC_DEBUG_CORE-only kernels), but
# default OFF: the old unconditional -DDEBUG force-enabled all sites at load
# and silently ruined performance measurements. `make DEBUG=1` restores it.
ccflags-y += -DDYNAMIC_DEBUG_MODULE
ifeq ($(DEBUG),1)
ccflags-y += -DDEBUG
endif

.PHONY: all clean modules modules_install

all: modules

modules:
ifeq ($(TBFIX_SYMVERS),)
	@echo "WARNING: no thunderbolt-tbfix Module.symvers found; thunderbolt_frame" >&2
	@echo "         built without it may fail to load (-22). Install/rebuild" >&2
	@echo "         thunderbolt-tbfix-dkms for this kernel first." >&2
endif
	$(MAKE) -C $(KDIR) M=$(PWD) $(EXTRA_SYMBOLS) modules

modules_install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
