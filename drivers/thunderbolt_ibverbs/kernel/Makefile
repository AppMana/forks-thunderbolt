# SPDX-License-Identifier: GPL-2.0
#
# Out-of-tree build for the release-track thunderbolt-ibverbs module.

KVER ?= $(shell uname -r)
KDIR ?= /lib/modules/$(KVER)/build
PWD := $(shell pwd)
TBFIX_SYMVERS ?= $(shell ls -1 /usr/src/thunderbolt-tbfix-*/Module.symvers 2>/dev/null | sort -V | tail -1)

# thunderbolt_ibverbs references our PATCHED core's exports (tb_ring_*,
# tb_xdomain_*). Without the tbfix core Module.symvers, MODVERSIONS records the
# wrong/absent CRCs and the module loads with "disagrees about version of
# symbol ... Unknown symbol (err -22)" -- a silent-at-build, dead-at-load
# failure. Building against the core symvers is not optional, so a missing one
# is a hard error (not a silent EXTRA_SYMBOLS-less build). thunderbolt-tbfix-dkms
# is a package Depends and its POST_INSTALL writes the symvers to
# /usr/src/thunderbolt-tbfix-<ver>/Module.symvers before ibverbs builds.
#
# NOTE (cross-kernel): that symvers file is a single per-source-version file, so
# if tbfix has not (re)built for THIS kernel yet, it can carry another kernel's
# CRCs. Order is enforced at the package layer (Depends) and, on a kernel
# update, both DKMS packages autoinstall for the new kernel; tbfix must build
# first. Verified matching on the fleet today; the new-kernel autoinstall
# ordering wants a hardware-validated roll to confirm.
ifneq ($(TBFIX_SYMVERS),)
EXTRA_SYMBOLS := KBUILD_EXTRA_SYMBOLS=$(TBFIX_SYMVERS)
endif

# Shadow the kernel's <linux/thunderbolt.h> with the fork-patched copy the top
# Makefile generated (source-aware handler additions), so ibverbs sees the same
# public struct layout as the core it links against. Prepended to LINUXINCLUDE
# ahead of the kernel include dirs. Empty when built without the tbfix shim or
# under the KUnit overlay (which patches the header directly).
ifneq ($(TBV_THUNDERBOLT_H_DIR),)
LINUXINCLUDE := -I$(TBV_THUNDERBOLT_H_DIR) $(LINUXINCLUDE)
endif

obj-m := thunderbolt_ibverbs.o
thunderbolt_ibverbs-y := main.o profile.o backend.o configfs.o core.o debugfs.o ibdev.o link.o link_owner.o native.o apple.o trace.o native_control.o native_control_legacy.o native_control_xdomain.o path.o peer.o rail.o service.o tbnet_identity.o tbnet_minimal.o ../proto/config.o ../proto/identity.o ../proto/reliability.o

# KUnit contract tests for driver logic (ACK routing, PSN, negotiation, naming,
# per-rail MAC). Only compiled in on a CONFIG_KUNIT kernel. Run them with
# drivers/thunderbolt_ibverbs/tools/run-kunit.sh (kunit.py against an overlaid
# v6.17 tree, x86_64 qemu) -- the fleet/host kernels lack CONFIG_KUNIT.
thunderbolt_ibverbs-$(CONFIG_KUNIT) += tests/ack_routing_test.o
thunderbolt_ibverbs-$(CONFIG_KUNIT) += tests/ibdev_naming_test.o
thunderbolt_ibverbs-$(CONFIG_KUNIT) += tests/rail_mac_test.o
thunderbolt_ibverbs-$(CONFIG_KUNIT) += tests/nccl_device_test.o
thunderbolt_ibverbs-$(CONFIG_KUNIT) += tests/post_recv_dispatch_test.o
thunderbolt_ibverbs-$(CONFIG_KUNIT) += tests/retransmit_backoff_test.o
thunderbolt_ibverbs-$(CONFIG_KUNIT) += tests/credit_pingpong_test.o
thunderbolt_ibverbs-$(CONFIG_KUNIT) += tests/send_timeout_arming_test.o
thunderbolt_ibverbs-$(CONFIG_KUNIT) += tests/timeout_reap_drain_test.o
thunderbolt_ibverbs-$(CONFIG_KUNIT) += tests/tx_stall_livelock_test.o
thunderbolt_ibverbs-$(CONFIG_KUNIT) += tests/retry_enomem_bound_test.o
thunderbolt_ibverbs-$(CONFIG_KUNIT) += tests/rnr_credit_wait_test.o
thunderbolt_ibverbs-$(CONFIG_KUNIT) += tests/credit_stall_recovery_test.o
thunderbolt_ibverbs-$(CONFIG_KUNIT) += tests/async_event_test.o
thunderbolt_ibverbs-$(CONFIG_KUNIT) += tests/cq_overflow_deadlock_test.o
thunderbolt_ibverbs-$(CONFIG_KUNIT) += tests/error_completion_test.o
thunderbolt_ibverbs-$(CONFIG_KUNIT) += tests/completion_accounting_test.o
thunderbolt_ibverbs-$(CONFIG_KUNIT) += tests/rx_hw_error_test.o
thunderbolt_ibverbs-$(CONFIG_KUNIT) += tests/zcopy_split_test.o
thunderbolt_ibverbs-$(CONFIG_KUNIT) += tests/nak_protocol_test.o
thunderbolt_ibverbs-$(CONFIG_KUNIT) += tests/send_wc_completion_test.o
thunderbolt_ibverbs-$(CONFIG_KUNIT) += tests/reload_quiesce_test.o
thunderbolt_ibverbs-$(CONFIG_KUNIT) += tests/create_qp_rail_bind_test.o
thunderbolt_ibverbs-$(CONFIG_KUNIT) += tests/qp_dgid_rebind_test.o
thunderbolt_ibverbs-$(CONFIG_KUNIT) += tests/qp_first_connect_test.o
thunderbolt_ibverbs-$(CONFIG_KUNIT) += tests/link_owner_test.o
thunderbolt_ibverbs-$(CONFIG_KUNIT) += tests/tx_ring_stall_test.o

ccflags-y := -Wall -Wextra -Wno-unused-parameter -DDEBUG -I$(src) -I$(src)/..

# Package version from dkms.conf, passed down by the top-level Makefile; absent
# on overlay/KUnit builds, where MODULE_VERSION compiles out.
ifneq ($(TBV_PKG_VERSION),)
ccflags-y += -DTBV_PKG_VERSION=\"$(TBV_PKG_VERSION)\"
endif

.PHONY: all clean modules modules_install help

all: modules

# A missing tbfix core symvers is only a WARNING, not a hard failure: the nix
# flake and local overlay builds resolve the core exports differently, and the
# KUnit harness generates its own Makefile. thunderbolt-tbfix-dkms is a package
# Depends so the DKMS/apt path always has it; this catches a hand-run build that
# forgot it (the module would otherwise build clean and -22 at load).
modules:
ifeq ($(TBFIX_SYMVERS),)
	@echo "WARNING: no thunderbolt-tbfix Module.symvers under /usr/src/thunderbolt-tbfix-*/;" >&2
	@echo "         thunderbolt_ibverbs built without it may fail to load (-22). Install/" >&2
	@echo "         rebuild thunderbolt-tbfix-dkms for this kernel first." >&2
endif
	$(MAKE) -C $(KDIR) M=$(PWD) $(EXTRA_SYMBOLS) modules

modules_install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	$(RM) ../proto/*.o ../proto/.*.o.cmd

help:
	@echo "Targets:"
	@echo "  modules            Build thunderbolt_ibverbs.ko"
	@echo "  modules_install    Install under /lib/modules/\$$(uname -r)/extra/"
	@echo "  clean              Remove build artifacts"
	@echo ""
	@echo "Parameters:"
	@echo "  compat=auto|force|off"
	@echo "  profile=auto|mac_compat|linux_perf|mixed"
	@echo "  tbnet=auto|allow|prefer_rdma|block"
	@echo "  tbnet_identity=auto|stock|stock_proxy|minimal_packet|off"
	@echo "  tbnet_identity_tbnet=<netdev>"
	@echo "  tbnet_identity_gid=auto|<netdev>"
	@echo "  tbnet_identity_minimal_e2e=0|1"
	@echo "  tbnet_identity_minimal_apple_only=0|1"
	@echo "  bind_services=0|1"
	@echo "  allocate_rings=0|1"
	@echo "  start_rings=0|1"
	@echo "  negotiate_native=0|1"
	@echo "  enable_tunnels=0|1"
	@echo "  register_verbs=0|1"
	@echo "  native_prtcstns=<u32>"
	@echo "  apple_prtcstns=<u32>"
	@echo "  apple_tx_max_inflight_wr=<N>"
	@echo "  apple_tx_max_inflight_frames=<N>"
	@echo "  apple_rx_pending_bytes=<bytes>"
	@echo "  apple_rx_pending_slots=<N>"
	@echo "  apple_rx_pending_total_bytes=<bytes>"
	@echo "  apple_rx_trace=<N>"
