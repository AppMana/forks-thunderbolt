# SPDX-License-Identifier: GPL-2.0
#
# Out-of-tree build for tbrxe.ko: the rxe-derived IB engine over the tbframe
# frame service. Modeled on ../Makefile (thunderbolt_ibverbs).
#
# Symbol tables:
#  - ib_core symbols come from the distro kernel's own Module.symvers (in
#    KDIR); nothing extra is needed to compile.
#  - tbframe_* symbols come from ../tbframe/Module.symvers. tbframe builds
#    in parallel with this scaffold, so a missing symvers is tolerated: the
#    build then finishes with modpost "undefined!" warnings and the module
#    simply requires tbframe.ko at load time. Override with
#    TBFRAME_SYMVERS=/path/to/Module.symvers once tbframe packaging exists.

KVER ?= $(shell uname -r)
KDIR ?= /lib/modules/$(KVER)/build
PWD := $(shell pwd)

TBFRAME_SYMVERS ?= $(wildcard $(PWD)/../tbframe/Module.symvers)
ifneq ($(TBFRAME_SYMVERS),)
EXTRA_SYMBOLS := KBUILD_EXTRA_SYMBOLS=$(TBFRAME_SYMVERS)
else
# tbframe builds in parallel and may not have a Module.symvers yet; without
# it modpost would hard-error on the tbframe_* references. Downgrade to
# warnings so the scaffold compiles standalone (the module then simply
# requires tbframe.ko at load time). Packaged builds set TBFRAME_SYMVERS.
EXTRA_SYMBOLS := KBUILD_MODPOST_WARN=1
endif

obj-m := tbrxe.o

# Engine and verbs objects: upstream rxe file set minus rxe_net.o, plus the
# tbframe transport (tbrxe_frame.o). rxe_req/rxe_comp/rxe_resp, rxe_task,
# rxe_opcode and the queue/pool/mr machinery are kept verbatim (see
# tbrxe_frame.c for the two documented engine seams).
tbrxe-y := rxe.o rxe_comp.o rxe_req.o rxe_resp.o rxe_recv.o rxe_pool.o \
	rxe_queue.o rxe_verbs.o rxe_av.o rxe_srq.o rxe_qp.o rxe_cq.o \
	rxe_mr.o rxe_opcode.o rxe_mmap.o rxe_icrc.o rxe_mcast.o rxe_task.o \
	rxe_mw.o rxe_hw_counters.o tbrxe_frame.o

tbrxe-$(CONFIG_INFINIBAND_ON_DEMAND_PAGING) += rxe_odp.o

# The production tbframe binding references tbframe_* exports and is
# excluded from KUnit kernels (built-in undefined symbols fail the vmlinux
# link there); tests install mock transport ops instead.
ifneq ($(CONFIG_KUNIT),y)
tbrxe-y += tbrxe_tbframe_glue.o
endif

# KUnit smoke test: one RC SEND through requester -> loopback -> responder
# -> completer with a null/mock tbframe. Only compiled on a CONFIG_KUNIT
# kernel; run it with tests/run-kunit.sh (the fleet/host kernels lack
# CONFIG_KUNIT).
tbrxe-$(CONFIG_KUNIT) += tests/tbrxe_loopback_test.o

ccflags-y := -Wall -Wextra -Wno-unused-parameter -DDEBUG -I$(src)

ifneq ($(TBV_PKG_VERSION),)
ccflags-y += -DTBV_PKG_VERSION=\"$(TBV_PKG_VERSION)\"
endif

.PHONY: all clean modules modules_install help

all: modules

modules:
	$(MAKE) -C $(KDIR) M=$(PWD) $(EXTRA_SYMBOLS) modules

modules_install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

help:
	@echo "Targets:"
	@echo "  modules            Build tbrxe.ko"
	@echo "  modules_install    Install under /lib/modules/\$$(uname -r)/extra/"
	@echo "  clean              Remove build artifacts"
	@echo ""
	@echo "Variables:"
	@echo "  KVER=<version>               Target kernel (default: uname -r)"
	@echo "  TBFRAME_SYMVERS=<path>       tbframe Module.symvers (optional until"
	@echo "                               tbframe builds; absent -> modpost warns)"
