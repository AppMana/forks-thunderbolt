# SPDX-License-Identifier: GPL-2.0
#
# Top-level wrapper for out-of-tree builds and DKMS.

KVER ?= $(shell uname -r)
KDIR ?= /lib/modules/$(KVER)/build
KERNEL_DIR ?= kernel

# MODULE_VERSION is stamped from dkms.conf's PACKAGE_VERSION -- the one place
# the package version is declared -- so modinfo -F version always matches the
# installed .deb. Empty (hand build outside this Makefile) compiles the tag out.
TBV_PKG_VERSION ?= $(shell sed -n 's/^PACKAGE_VERSION="\(.*\)"/\1/p' $(CURDIR)/dkms.conf)

# thunderbolt_ibverbs is built against the fork's PATCHED thunderbolt subsystem
# and references the same public-header additions the core does (callback_xd /
# TB_PROTOCOL_HANDLER_HAS_XDOMAIN, behind #ifdef). Generate the fork-patched
# <linux/thunderbolt.h> into a build-local dir and shadow the kernel's copy
# (kernel/Makefile prepends it to LINUXINCLUDE), self-contained -- no node
# header mutation. Single source of the transformation: reuse the shim shipped
# by thunderbolt-tbfix-dkms (a package Depends), found the same way as its
# Module.symvers. Absent (hand build without tbfix) -> no shadow, and the
# #ifdef'd source-aware paths compile out to the legacy handler.
TBFIX_HEADER_SHIM ?= $(shell ls -1 /usr/src/thunderbolt-tbfix-*/tbfix-gen-thunderbolt-header.sh 2>/dev/null | sort -V | tail -1)
TBV_THUNDERBOLT_H_DIR := $(CURDIR)/.tbfix-gen-include

.PHONY: all clean modules modules_install help dkms-add dkms-build dkms-install dkms-remove gen-header

all: modules

gen-header:
	@if [ -n "$(TBFIX_HEADER_SHIM)" ]; then \
		echo "  GEN     $(TBV_THUNDERBOLT_H_DIR)/linux/thunderbolt.h (via $(TBFIX_HEADER_SHIM))"; \
		"$(TBFIX_HEADER_SHIM)" "$(KDIR)" "$(TBV_THUNDERBOLT_H_DIR)"; \
	else \
		echo "  NOTE    no thunderbolt-tbfix header shim found; building against stock <linux/thunderbolt.h> (legacy source-blind handler)"; \
	fi

modules: gen-header
	$(MAKE) -C $(KERNEL_DIR) KVER=$(KVER) KDIR=$(KDIR) \
		TBV_PKG_VERSION=$(TBV_PKG_VERSION) \
		$(if $(TBFIX_HEADER_SHIM),TBV_THUNDERBOLT_H_DIR=$(TBV_THUNDERBOLT_H_DIR)) modules

modules_install:
	$(MAKE) -C $(KERNEL_DIR) KVER=$(KVER) KDIR=$(KDIR) modules_install

clean:
	$(MAKE) -C $(KERNEL_DIR) KVER=$(KVER) KDIR=$(KDIR) clean
	$(RM) -r $(TBV_THUNDERBOLT_H_DIR)

help:
	$(MAKE) -C $(KERNEL_DIR) help
	@echo ""
	@echo "Top-level targets:"
	@echo "  dkms-add          Register this source tree with DKMS"
	@echo "  dkms-build        Build with DKMS for KVER=$$(uname -r)"
	@echo "  dkms-install      Install with DKMS for KVER=$$(uname -r)"
	@echo "  dkms-remove       Remove this DKMS module version"

dkms-add:
	dkms add .

dkms-build:
	dkms build thunderbolt-ibverbs/0.1.0 -k $(KVER)

dkms-install:
	dkms install thunderbolt-ibverbs/0.1.0 -k $(KVER)

dkms-remove:
	dkms remove thunderbolt-ibverbs/0.1.0 --all
