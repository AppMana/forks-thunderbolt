#!/usr/bin/env bash
# Build THREE dependency-linked DKMS .debs from the one source tree:
#   thunderbolt-tbfix-core            -> thunderbolt.ko
#   thunderbolt-tbfix-net  (Dep core) -> thunderbolt_net.ko
#   thunderbolt-ibverbs    (Dep core) -> thunderbolt_ibverbs.ko
#
# Each package preserves the repo's drivers/ layout so the single canonical
# drivers/thunderbolt/thunderbolt_negotiation.h is reached by the same relative
# include used in-tree. The dependents bundle that one header (copied from the
# canonical -- a build artifact, not a second source) and link against the
# core's exported symbols by building the installed core source first to produce
# its Module.symvers (robust across kernel upgrades; no cross-package dkms-tree
# path guessing).
#
#   tools/ci/distro-package-split.sh ubuntu
set -euo pipefail

distro="${1:-}"
case "$distro" in debian|ubuntu) ;; *) echo "usage: $0 debian|ubuntu" >&2; exit 1;; esac

root="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
ver="${TBFIX_VERSION:-$(awk -F'"' '/^PACKAGE_VERSION=/{print $2; exit}' "$root/dkms/dkms.conf")}"
out="${OUT_DIR:-$root/dist}"; mkdir -p "$out"
sha="$(git -C "$root" rev-parse --short HEAD 2>/dev/null || echo unknown)"

# stage_module <pkg> <modname> <module-subdir-under-drivers> <dep> <needs_core>
stage_pkg() {
	local pkg="$1" mod="$2" sub="$3" dep="$4" needs_core="$5"
	local stage; stage="$(mktemp -d)"
	local src="$stage/usr/src/${pkg}-${ver}"
	install -d "$stage/DEBIAN" "$src/$(dirname "$sub")"
	# the module source, preserving drivers/ structure
	cp -a "$root/$sub" "$src/$sub"
	# ibverbs links ../proto/*.o -- bring its sibling proto/ along
	if [ -d "$root/$(dirname "$sub")/proto" ]; then
		cp -a "$root/$(dirname "$sub")/proto" "$src/$(dirname "$sub")/proto"
	fi
	# the dependents bundle the ONE canonical header at its in-tree path
	if [ "$needs_core" = 1 ]; then
		install -D -m0644 "$root/drivers/thunderbolt/thunderbolt_negotiation.h" \
			"$src/drivers/thunderbolt/thunderbolt_negotiation.h"
	fi

	# top-level Makefile dkms invokes
	if [ "$needs_core" = 1 ]; then
		cat > "$src/Makefile" <<MK
KDIR ?= /lib/modules/\$(shell uname -r)/build
# build the installed core source first to get its Module.symvers, then us
CORE := \$(firstword \$(wildcard /usr/src/thunderbolt-tbfix-core-*))
all:
	\$(MAKE) -C \$(KDIR) M=\$(CORE)/drivers/thunderbolt modules
	\$(MAKE) -C \$(KDIR) M=\$(CURDIR)/$sub KBUILD_EXTRA_SYMBOLS=\$(CORE)/drivers/thunderbolt/Module.symvers modules
clean:
	\$(MAKE) -C \$(KDIR) M=\$(CURDIR)/$sub clean
MK
	else
		cat > "$src/Makefile" <<MK
KDIR ?= /lib/modules/\$(shell uname -r)/build
all:
	\$(MAKE) -C \$(KDIR) M=\$(CURDIR)/$sub modules
clean:
	\$(MAKE) -C \$(KDIR) M=\$(CURDIR)/$sub clean
MK
	fi

	cat > "$src/dkms.conf" <<DK
PACKAGE_NAME="$pkg"
PACKAGE_VERSION="$ver"
BUILT_MODULE_NAME[0]="$mod"
BUILT_MODULE_LOCATION[0]="$sub"
DEST_MODULE_LOCATION[0]="/updates/dkms"
MAKE[0]="make KDIR=\${kernel_source_dir}"
CLEAN="make KDIR=\${kernel_source_dir} clean"
AUTOINSTALL="yes"
DK

	local depline=""
	[ -n "$dep" ] && depline=", $dep (= $ver)"
	cat > "$stage/DEBIAN/control" <<CT
Package: $pkg
Version: $ver
Section: kernel
Priority: optional
Architecture: all
Depends: dkms (>= 2.1.0.0), kmod, make, libc6$depline
Maintainer: AppMana <ops@appmana.com>
Homepage: https://github.com/AppMana/forks-thunderbolt
Description: AppMana patched $mod kernel module (DKMS, fork-sha $sha)
 DKMS source for AppMana's patched $mod.ko. Rebuilt for the running kernel.
CT
	cat > "$stage/DEBIAN/postinst" <<'PI'
#!/bin/sh
set -e
PI
	cat >> "$stage/DEBIAN/postinst" <<PI
NAME=$pkg; VER=$ver
dkms add -m \$NAME -v \$VER || true
dkms build -m \$NAME -v \$VER
dkms install -m \$NAME -v \$VER --force
PI
	cat > "$stage/DEBIAN/prerm" <<PR
#!/bin/sh
set -e
dkms remove -m $pkg -v $ver --all || true
PR
	chmod 0755 "$stage/DEBIAN/postinst" "$stage/DEBIAN/prerm"
	dpkg-deb --root-owner-group --build "$stage" "$out/${pkg}_${ver}_all.deb" >/dev/null
	echo "==> $out/${pkg}_${ver}_all.deb"
	rm -rf "$stage"
}

stage_pkg thunderbolt-tbfix-core thunderbolt           drivers/thunderbolt            ""                        0
stage_pkg thunderbolt-tbfix-net  thunderbolt_net       drivers/net/thunderbolt        thunderbolt-tbfix-core    1
stage_pkg thunderbolt-ibverbs    thunderbolt_ibverbs   drivers/thunderbolt_ibverbs/kernel thunderbolt-tbfix-core 1
