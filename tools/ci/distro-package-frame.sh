#!/usr/bin/env bash
# Build the thunderbolt-frame DKMS source package (thunderbolt_frame.ko +
# thunderbolt_frame_rxe.ko in ONE deb) for Debian/Ubuntu. The package rides
# the tbfix version stream (dkms/dkms.conf PACKAGE_VERSION) and the produced
# deb depends on thunderbolt-tbfix-dkms (= version).

set -euo pipefail

usage() {
	cat <<'USAGE'
Usage:
  tools/ci/distro-package-frame.sh debian|ubuntu

Outputs thunderbolt-frame-dkms_<version>_all.deb (+ .sha256) into OUT_DIR.

Environment:
  TBFIX_VERSION   Override package version. Defaults to PACKAGE_VERSION in
                  dkms/dkms.conf (the tbfix version stream -- lockstep).
  OUT_DIR         Output directory. Defaults to $PWD/dist.
  WORK_DIR        Scratch directory. Defaults to mktemp.
  TBFIX_LINT      Run lintian if available. Defaults to 0.
  TBFIX_SKIP_DEPS Skip apt dependency install. Defaults to 0.
USAGE
}

distro="${1:-}"
case "${distro:-}" in
	-h|--help) usage; exit 0 ;;
	debian|ubuntu) ;;
	"") usage >&2; exit 1 ;;
	*) printf 'error: unsupported distro: %s\n' "$distro" >&2; exit 1 ;;
esac

repo_root="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
# ONE authoritative version: the tbfix stream. The staged dkms.conf is stamped
# below so the DKMS identity, the deb version and the tbfix dependency all
# carry the same number.
version="${TBFIX_VERSION:-$(awk -F'"' '/^PACKAGE_VERSION=/ { print $2; exit }' "$repo_root/dkms/dkms.conf")}"
[[ -n "$version" ]] || { printf 'error: could not determine version from dkms/dkms.conf\n' >&2; exit 1; }

out_dir="${OUT_DIR:-$repo_root/dist}"
work_dir="${WORK_DIR:-$(mktemp -d)}"
lint="${TBFIX_LINT:-0}"
skip_deps="${TBFIX_SKIP_DEPS:-0}"
modname="thunderbolt-frame"
pkgname="${modname}-dkms"
frame_root="$repo_root/drivers/thunderbolt_frame"

mkdir -p "$out_dir" "$work_dir"

install_deps() {
	[[ "$skip_deps" == "1" ]] && return 0
	export DEBIAN_FRONTEND=noninteractive
	apt-get update -qq
	apt-get install -y -qq --no-install-recommends \
		ca-certificates dpkg-dev fakeroot lintian
}

stage_source() {
	local stage="$1"
	local contaminant
	install -d -m 0755 "$stage/kernel" "$stage/thunderbolt"
	install -m 0644 "$repo_root/dkms/frame/dkms.conf" "$stage/dkms.conf"
	sed -i "s/^PACKAGE_VERSION=.*/PACKAGE_VERSION=\"$version\"/" \
		"$stage/dkms.conf"
	install -m 0644 "$repo_root/dkms/frame/Makefile" "$stage/Makefile"
	# A developer package is commonly built right after `make` in the module
	# dirs. Never stage those host-kernel objects into an arch-independent
	# DKMS source package: DKMS must rebuild pure source against the target
	# kernel, the installed tbfix header shim/symvers, and the frame
	# module's own fresh symvers.
	tar -C "$frame_root" \
		--exclude='*.o' \
		--exclude='*.ko' \
		--exclude='*.mod' \
		--exclude='*.mod.c' \
		--exclude='*.cmd' \
		--exclude='Module.symvers' \
		--exclude='modules.order' \
		--exclude='.tmp_*' \
		--exclude='.tbfix-gen-include' \
		-cf - frame rxe \
		| tar -C "$stage/kernel" -xf -
	# frame/tbframe_priv.h includes ../../thunderbolt/thunderbolt_negotiation.h.
	# Bundle the ONE canonical header (drivers/thunderbolt/) at that relative
	# location -- a build artifact copied from the canonical, not a second
	# source. kernel/frame/../../thunderbolt/ == $stage/thunderbolt/.
	install -m 0644 "$repo_root/drivers/thunderbolt/thunderbolt_negotiation.h" \
		"$stage/thunderbolt/thunderbolt_negotiation.h"
	contaminant="$(find "$stage" -type f \( \
		-name '*.o' -o -name '*.ko' -o -name '*.mod' -o \
		-name '*.mod.c' -o -name '*.cmd' -o \
		-name 'Module.symvers' -o -name 'modules.order' \
		\) -print -quit)"
	[[ -z "$contaminant" ]] || {
		printf 'error: build artefact escaped DKMS source filter: %s\n' \
			"$contaminant" >&2
		exit 1
	}
	{
		printf '# Auto-generated package source metadata\n'
		printf 'fork-sha=%s\n' "$(git -C "$repo_root" rev-parse HEAD 2>/dev/null || printf unknown)"
		printf 'fork-describe=%s\n' "$(git -C "$repo_root" describe --always --dirty 2>/dev/null || printf unknown)"
		printf 'packaged-at=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
	} > "$stage/.thunderbolt-frame-source"
	# Worktrees may be group-writable. A binary package must not preserve
	# developer umask/checkout permissions in /usr/src.
	chmod -R a+rX,u+w,go-w "$stage"
}

substitute() {
	sed "s/@VERSION@/${version}/g" "$1" > "$2"
}

build_deb() {
	local stage="$work_dir/deb"
	rm -rf "$stage"
	install -d -m 0755 "$stage/DEBIAN"
	stage_source "$stage/usr/src/${modname}-${version}"

	substitute "$repo_root/packaging/debian/control-frame" "$stage/DEBIAN/control"
	substitute "$repo_root/packaging/debian/postinst-frame" "$stage/DEBIAN/postinst"
	substitute "$repo_root/packaging/debian/prerm-frame" "$stage/DEBIAN/prerm"
	chmod 0755 "$stage/DEBIAN/postinst" "$stage/DEBIAN/prerm"

	local deb="$out_dir/${pkgname}_${version}_all.deb"
	dpkg-deb --root-owner-group --build "$stage" "$deb" >/dev/null
	(
		cd "$out_dir"
		sha256sum "$(basename "$deb")" > "$(basename "$deb").sha256"
	)
	printf '==> Built %s\n' "$deb"

	if [[ "$lint" == "1" ]] && command -v lintian >/dev/null 2>&1; then
		lintian --no-tag-display-limit \
			--suppress-tags no-changelog,no-manual-page,no-copyright-file,extended-description-is-probably-too-short,initial-upload-closes-no-bugs,debian-changelog-file-missing \
			"$deb" || true
	fi
}

install_deps
build_deb
