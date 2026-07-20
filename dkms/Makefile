KDIR ?= /lib/modules/$(shell uname -r)/build
TBFIX_SOURCE_DIR ?= $(shell readlink -f $(CURDIR)/../source 2>/dev/null || pwd)

# MODULE_VERSION is stamped from dkms.conf's PACKAGE_VERSION -- the one place
# the package version is declared -- so modinfo -F version always matches the
# installed .deb. Empty (hand build outside this Makefile) compiles the tag out.
TBV_PKG_VERSION ?= $(shell sed -n 's/^PACKAGE_VERSION="\(.*\)"/\1/p' $(CURDIR)/dkms.conf)

# Build-local directory holding the fork-patched <linux/thunderbolt.h>,
# generated from the kernel's own header (KDIR) so it tracks the stock header
# per kernel and adds only the members the vendored subsystem needs. Prepended
# to LINUXINCLUDE in each module Makefile so it shadows the kernel's copy. This
# is why the package no longer depends on out-of-band kernel-header mutation.
TBV_THUNDERBOLT_H_DIR := $(CURDIR)/.tbfix-gen-include

.PHONY: all clean gen-header

# Source is staged preserving the real drivers/ tree so that net's in-tree
# include "../../thunderbolt/thunderbolt_negotiation.h" (the one canonical
# negotiation header shared by core and net) resolves exactly as it does
# in-tree. net links against the core's freshly built Module.symvers.
all: gen-header
	$(MAKE) -C $(KDIR) M=$(CURDIR)/drivers/thunderbolt \
		TBV_THUNDERBOLT_H_DIR=$(TBV_THUNDERBOLT_H_DIR) \
		TBV_PKG_VERSION=$(TBV_PKG_VERSION) modules
	cp $(CURDIR)/drivers/thunderbolt/Module.symvers $(CURDIR)/Module.symvers
	cp $(CURDIR)/drivers/thunderbolt/Module.symvers $(TBFIX_SOURCE_DIR)/Module.symvers
	$(MAKE) -C $(KDIR) M=$(CURDIR)/drivers/net/thunderbolt \
		TBV_THUNDERBOLT_H_DIR=$(TBV_THUNDERBOLT_H_DIR) \
		TBV_PKG_VERSION=$(TBV_PKG_VERSION) \
		KBUILD_EXTRA_SYMBOLS=$(CURDIR)/drivers/thunderbolt/Module.symvers modules
	$(MAKE) -C $(KDIR) M=$(CURDIR)/drivers/infiniband/sw/rxe \
		TBV_PKG_VERSION=$(TBV_PKG_VERSION) modules

gen-header:
	$(CURDIR)/tbfix-gen-thunderbolt-header.sh "$(KDIR)" "$(TBV_THUNDERBOLT_H_DIR)"

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR)/drivers/thunderbolt clean
	$(MAKE) -C $(KDIR) M=$(CURDIR)/drivers/net/thunderbolt clean
	$(MAKE) -C $(KDIR) M=$(CURDIR)/drivers/infiniband/sw/rxe clean
	$(RM) $(CURDIR)/Module.symvers
	$(RM) -r $(TBV_THUNDERBOLT_H_DIR)
