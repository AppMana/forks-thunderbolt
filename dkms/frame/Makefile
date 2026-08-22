# SPDX-License-Identifier: GPL-2.0
#
# DKMS top-level build for the thunderbolt-frame package: thunderbolt_frame.ko
# then thunderbolt_frame_rxe.ko in one pass. The per-module Makefiles under
# kernel/ are the same ones used for developer builds (frame/ and rxe/ in the
# repo); this file only sequences them and pins the cross-module coupling:
#
#   tbfix core Module.symvers ──> thunderbolt_frame ── Module.symvers ──> thunderbolt_frame_rxe
#
# thunderbolt_frame resolves the tbfix core symbols itself (kernel/frame's
# Makefile prefers the per-kernel /var/lib/dkms/thunderbolt-tbfix/*/$(KVER)
# table); the rxe engine gets the frame module's freshly written symvers
# passed explicitly so it can never pair with a stale build.

KVER ?= $(shell uname -r)
KDIR ?= /lib/modules/$(KVER)/build

# MODULE_VERSION stamp, from dkms.conf's PACKAGE_VERSION -- itself stamped from
# the tbfix version stream at package build. Empty compiles the tag out.
TBV_PKG_VERSION ?= $(shell sed -n 's/^PACKAGE_VERSION="\(.*\)"/\1/p' $(CURDIR)/dkms.conf)

# The tbfix header shim script ships in the installed thunderbolt-tbfix-dkms
# source tree (an exact-version package dependency). Generate the shim ONCE
# here and hand it to the frame build. Building the frame service source-blind
# against the stock <linux/thunderbolt.h> is the NX-execute-panic class
# (appmana-025, kdump 202608031305), so a missing script is a hard error --
# never the per-module Makefile's warn-and-continue fallback.
TBFIX_HDR_GEN := $(shell ls -1 /usr/src/thunderbolt-tbfix-*/tbfix-gen-thunderbolt-header.sh 2>/dev/null | sort -V | tail -1)
TBV_THUNDERBOLT_H_DIR := $(CURDIR)/.tbfix-gen-include

.PHONY: all clean preflight

all: preflight
	$(MAKE) -C $(CURDIR)/kernel/frame KVER=$(KVER) KDIR=$(KDIR) \
		TBV_THUNDERBOLT_H_DIR=$(TBV_THUNDERBOLT_H_DIR) \
		TBV_PKG_VERSION=$(TBV_PKG_VERSION) modules
	$(MAKE) -C $(CURDIR)/kernel/rxe KVER=$(KVER) KDIR=$(KDIR) \
		TBFRAME_SYMVERS=$(CURDIR)/kernel/frame/Module.symvers \
		TBV_PKG_VERSION=$(TBV_PKG_VERSION) modules

# Fail fast, with the reason, instead of building a module that -22s at load
# (missing tbfix symvers) or panics (missing header shim).
preflight:
	@test -n "$(TBFIX_HDR_GEN)" || { \
		echo "error: tbfix-gen-thunderbolt-header.sh not found under /usr/src/thunderbolt-tbfix-*/;" >&2; \
		echo "       install thunderbolt-tbfix-dkms (exact-version dependency) first." >&2; \
		exit 1; }
	@ls /var/lib/dkms/thunderbolt-tbfix/*/$(KVER)/*/module/Module.symvers >/dev/null 2>&1 || \
	 ls /usr/src/thunderbolt-tbfix-*/Module.symvers >/dev/null 2>&1 || { \
		echo "error: no thunderbolt-tbfix Module.symvers for $(KVER);" >&2; \
		echo "       build/install thunderbolt-tbfix-dkms for this kernel first." >&2; \
		exit 1; }
	@ls /lib/modules/$(KVER)/kernel/drivers/infiniband/core/ib_uverbs.ko* \
	    >/dev/null 2>&1 || { \
		echo "error: ib_uverbs.ko not present for $(KVER); thunderbolt_frame_rxe" >&2; \
		echo "       imports ib_core/ib_umem symbols that Ubuntu ships in" >&2; \
		echo "       linux-modules-extra-$(KVER). Install it first." >&2; \
		exit 1; }
	$(TBFIX_HDR_GEN) "$(KDIR)" "$(TBV_THUNDERBOLT_H_DIR)"

clean:
	$(MAKE) -C $(CURDIR)/kernel/frame KVER=$(KVER) KDIR=$(KDIR) clean
	$(MAKE) -C $(CURDIR)/kernel/rxe KVER=$(KVER) KDIR=$(KDIR) clean
	$(RM) -r $(TBV_THUNDERBOLT_H_DIR)
