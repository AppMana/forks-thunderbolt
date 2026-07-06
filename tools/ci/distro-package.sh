#!/usr/bin/env bash
# Build the thunderbolt-tbfix DKMS source package for Debian/Ubuntu.

set -euo pipefail

usage() {
	cat <<'EOF'
Usage:
  tools/ci/distro-package.sh debian|ubuntu

Outputs thunderbolt-tbfix-dkms_<version>_all.deb into OUT_DIR.

Environment:
  TBFIX_VERSION   Override package version. Defaults to PACKAGE_VERSION in dkms/dkms.conf.
  OUT_DIR         Output directory. Defaults to $PWD/dist.
  WORK_DIR        Scratch directory. Defaults to mktemp.
  TBFIX_LINT      Run lintian if available. Defaults to 0.
  TBFIX_SKIP_DEPS Skip apt dependency install. Defaults to 0.
EOF
}

distro="${1:-}"
case "${distro:-}" in
	-h|--help) usage; exit 0 ;;
	debian|ubuntu) ;;
	"") usage >&2; exit 1 ;;
	*) printf 'error: unsupported distro: %s\n' "$distro" >&2; exit 1 ;;
esac

repo_root="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
version="${TBFIX_VERSION:-$(awk -F'"' '/^PACKAGE_VERSION=/ { print $2; exit }' "$repo_root/dkms/dkms.conf")}"
[[ -n "$version" ]] || { printf 'error: could not determine version from dkms/dkms.conf\n' >&2; exit 1; }

out_dir="${OUT_DIR:-$repo_root/dist}"
work_dir="${WORK_DIR:-$(mktemp -d)}"
lint="${TBFIX_LINT:-0}"
skip_deps="${TBFIX_SKIP_DEPS:-0}"
modname="thunderbolt-tbfix"
pkgname="${modname}-dkms"

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
	install -d -m 0755 "$stage"
	install -m 0644 "$repo_root/dkms/dkms.conf" "$stage/dkms.conf"
	install -m 0644 "$repo_root/dkms/Makefile" "$stage/Makefile"
	# Preserve the real drivers/ tree so net's in-tree include of the one
	# canonical ../../thunderbolt/thunderbolt_negotiation.h resolves as in-tree.
	install -d -m 0755 "$stage/drivers/net" "$stage/drivers/infiniband/sw"
	tar -C "$repo_root/drivers/thunderbolt" -cf - . | tar -C "$stage/drivers" -xf - --one-top-level=thunderbolt
	tar -C "$repo_root/drivers/net/thunderbolt" -cf - . | tar -C "$stage/drivers/net" -xf - --one-top-level=thunderbolt
	tar -C "$repo_root/drivers/infiniband/sw/rxe" -cf - . | tar -C "$stage/drivers/infiniband/sw" -xf - --one-top-level=rxe
	{
		printf '# Auto-generated package source metadata\n'
		printf 'fork-sha=%s\n' "$(git -C "$repo_root" rev-parse HEAD 2>/dev/null || printf unknown)"
		printf 'fork-describe=%s\n' "$(git -C "$repo_root" describe --always --dirty 2>/dev/null || printf unknown)"
		printf 'packaged-at=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
	} > "$stage/.tbfix-source"
}

substitute() {
	sed "s/@VERSION@/${version}/g" "$1" > "$2"
}

build_deb() {
	local stage="$work_dir/deb"
	rm -rf "$stage"
	install -d -m 0755 "$stage/DEBIAN"
	stage_source "$stage/usr/src/${modname}-${version}"

	substitute "$repo_root/packaging/debian/control" "$stage/DEBIAN/control"
	substitute "$repo_root/packaging/debian/postinst" "$stage/DEBIAN/postinst"
	substitute "$repo_root/packaging/debian/prerm" "$stage/DEBIAN/prerm"
	chmod 0755 "$stage/DEBIAN/postinst" "$stage/DEBIAN/prerm"

	local deb="$out_dir/${pkgname}_${version}_all.deb"
	dpkg-deb --root-owner-group --build "$stage" "$deb" >/dev/null
	sha256sum "$deb" > "$deb.sha256"
	printf '==> Built %s\n' "$deb"

	if [[ "$lint" == "1" ]] && command -v lintian >/dev/null 2>&1; then
		lintian --no-tag-display-limit \
			--suppress-tags no-changelog,no-manual-page,no-copyright-file,extended-description-is-probably-too-short,initial-upload-closes-no-bugs,debian-changelog-file-missing \
			"$deb" || true
	fi
}

install_deps
build_deb
