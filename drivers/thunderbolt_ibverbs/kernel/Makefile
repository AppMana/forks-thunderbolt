# SPDX-License-Identifier: GPL-2.0
#
# Out-of-tree build for the release-track thunderbolt-ibverbs module.

KVER ?= $(shell uname -r)
KDIR ?= /lib/modules/$(KVER)/build
PWD := $(shell pwd)
TBFIX_SYMVERS ?= $(shell ls -1 /usr/src/thunderbolt-tbfix-*/Module.symvers 2>/dev/null | sort -V | tail -1)

ifneq ($(TBFIX_SYMVERS),)
EXTRA_SYMBOLS := KBUILD_EXTRA_SYMBOLS=$(TBFIX_SYMVERS)
endif

obj-m := thunderbolt_ibverbs.o
thunderbolt_ibverbs-y := main.o profile.o backend.o configfs.o core.o debugfs.o ibdev.o link.o native.o apple.o trace.o native_control.o native_control_legacy.o native_control_xdomain.o path.o peer.o rail.o service.o tbnet_identity.o tbnet_minimal.o ../proto/config.o ../proto/identity.o ../proto/reliability.o

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

ccflags-y := -Wall -Wextra -Wno-unused-parameter -DDEBUG -I$(src) -I$(src)/..

.PHONY: all clean modules modules_install help

all: modules

modules:
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
