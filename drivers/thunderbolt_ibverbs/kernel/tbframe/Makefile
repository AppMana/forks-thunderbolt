# SPDX-License-Identifier: GPL-2.0
#
# Out-of-tree build for the tbframe lossless frame service.
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
# mechanism as ../Makefile. Empty when built without the tbfix shim; the
# #ifdef'd source-aware paths then compile out.
ifneq ($(TBV_THUNDERBOLT_H_DIR),)
LINUXINCLUDE := -I$(TBV_THUNDERBOLT_H_DIR) $(LINUXINCLUDE)
endif

obj-m := tbframe.o
tbframe-y := main.o core.o service.o hw.o

# KUnit contract tests against the mock ring layer (tbframe_hw_ops). Run
# with drivers/thunderbolt_ibverbs/tools/run-kunit.sh; host kernels lack
# CONFIG_KUNIT.
tbframe-$(CONFIG_KUNIT) += tests/session_test.o
tbframe-$(CONFIG_KUNIT) += tests/supersede_test.o
tbframe-$(CONFIG_KUNIT) += tests/window_test.o
tbframe-$(CONFIG_KUNIT) += tests/teardown_test.o

ccflags-y := -Wall -Wextra -Wno-unused-parameter -DDEBUG -I$(src)

.PHONY: all clean modules modules_install

all: modules

modules:
ifeq ($(TBFIX_SYMVERS),)
	@echo "WARNING: no thunderbolt-tbfix Module.symvers found; tbframe built" >&2
	@echo "         without it may fail to load (-22). Install/rebuild" >&2
	@echo "         thunderbolt-tbfix-dkms for this kernel first." >&2
endif
	$(MAKE) -C $(KDIR) M=$(PWD) $(EXTRA_SYMBOLS) modules

modules_install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
