#!/usr/bin/env bash
# Build the architecture-independent reproduction-tools package (the bounded
# tbv-hang-repro capture harness plus read-only NHI register diagnostics).
# Run inside a matching distro container (debian:sid / ubuntu). The script
# installs the build dependencies it needs.

set -euo pipefail

usage() {
	cat <<'EOF'
Usage:
  tools/ci/distro-package.sh debian|ubuntu

Outputs the produced tools .deb into OUT_DIR.

Environment:
  TBV_VERSION     Override package version. Defaults to packaging/version
                  (the userspace package stream shared with
                  usb4-rdma-provider).
  OUT_DIR         Output directory. Defaults to $(pwd)/dist.
  WORK_DIR        Scratch directory. Defaults to a fresh mktemp dir.
  TBV_LINT        Run lintian on the artefact. 1 to enable. Defaults to 0.
  TBV_SKIP_DEPS   Skip the distro deps install step. 1 to skip.
EOF
}

distro="${1:-}"
case "${distro:-}" in
	-h|--help) usage; exit 0 ;;
	debian|ubuntu) ;;
	"") usage >&2; exit 1 ;;
	*) printf 'error: unsupported distro: %s\n' "$distro" >&2; exit 1 ;;
esac

repo_root="$(CDPATH='' cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
version="${TBV_VERSION:-$(tr -d '[:space:]' < "$repo_root/packaging/version")}"
[[ -n "$version" ]] || { printf 'error: could not determine version from packaging/version\n' >&2; exit 1; }
out_dir="${OUT_DIR:-$repo_root/dist}"
work_dir="${WORK_DIR:-$(mktemp -d)}"
lint="${TBV_LINT:-0}"
skip_deps="${TBV_SKIP_DEPS:-0}"

mkdir -p "$out_dir" "$work_dir"

install_deps() {
	[[ "$skip_deps" == "1" ]] && return 0
	export DEBIAN_FRONTEND=noninteractive
	apt-get update -qq
	apt-get install -y -qq --no-install-recommends \
		ca-certificates dpkg-dev fakeroot lintian
}

substitute() {
	sed "s/@VERSION@/${version}/g" "$1" > "$2"
}

build_tools_deb() {
	local tools_stage="$work_dir/deb-tools"
	rm -rf "$tools_stage"
	install -d -m 0755 "$tools_stage/DEBIAN"
	substitute "$repo_root/packaging/debian/control-tools" \
		"$tools_stage/DEBIAN/control"
	install -D -m 0755 "$repo_root/tools/tbv-hang-repro.sh" \
		"$tools_stage/usr/bin/tbv-hang-repro"
	install -D -m 0755 "$repo_root/tools/nhi-ring-regs.py" \
		"$tools_stage/usr/bin/tbv-nhi-ring-regs"
	install -D -m 0644 "$repo_root/docs/tbv-hang-repro.md" \
		"$tools_stage/usr/share/doc/thunderbolt-frame-tools/README.md"

	local tools_deb="$out_dir/thunderbolt-frame-tools_${version}_all.deb"
	dpkg-deb --root-owner-group --build "$tools_stage" "$tools_deb" >/dev/null
	printf '==> Built %s\n' "$tools_deb"

	if [[ "$lint" == "1" ]] && command -v lintian >/dev/null 2>&1; then
		printf '==> lintian\n'
		lintian --no-tag-display-limit \
			--suppress-tags no-changelog,no-manual-page,no-copyright-file,extended-description-is-probably-too-short,initial-upload-closes-no-bugs,debian-changelog-file-missing \
			"$tools_deb" || true
	fi
}

install_deps
build_tools_deb
