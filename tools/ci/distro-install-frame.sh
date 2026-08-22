#!/usr/bin/env bash
# Install and verify a thunderbolt-frame-dkms .deb without loading modules.
# The package depends on the exact-version thunderbolt-tbfix-dkms, so that deb
# is installed (and, when compiling, dkms-built) first. By default this
# verifies package/source staging only. Set TBFIX_VERIFY_DKMS_BUILD=1 in an
# environment with matching 7.0 kernel headers to compile all modules too.

set -euo pipefail

tbfix_target="${1:-}"
frame_target="${2:-}"
if [[ -z "$tbfix_target" || -z "$frame_target" || "$tbfix_target" == "-h" || "$tbfix_target" == "--help" ]]; then
	cat <<'USAGE'
Usage:
  tools/ci/distro-install-frame.sh <thunderbolt-tbfix-dkms.deb> <thunderbolt-frame-dkms.deb>

Environment:
  TBFIX_VERIFY_DKMS_BUILD=1  Also run the dkms builds against installed headers.
USAGE
	exit 1
fi

resolve_one() {
	local pattern="$1" out
	shopt -s nullglob
	# shellcheck disable=SC2206
	local matches=( $pattern )
	shopt -u nullglob
	if [[ ${#matches[@]} -ne 1 ]]; then
		printf 'error: expected exactly one artefact for %s, got %d\n' \
			"$pattern" "${#matches[@]}" >&2
		exit 1
	fi
	out="$(realpath "${matches[0]}")"
	[[ -f "$out" ]] || { printf 'error: not a file: %s\n' "$out" >&2; exit 1; }
	printf '%s\n' "$out"
}

tbfix_deb="$(resolve_one "$tbfix_target")"
frame_deb="$(resolve_one "$frame_target")"

export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
headers_pkg=linux-headers-amd64
if grep -qi '^ID=ubuntu' /etc/os-release; then
    # Same 7.0-headers selection rationale as tools/ci/distro-install.sh.
    headers_pkg="${TBFIX_HEADERS_PKG:-$(apt-cache search --names-only '^linux-headers-7[.]0[.]0-[0-9]+-generic$' |
        awk '{print $1}' | sort -V | tail -1)}"
    [[ -n "$headers_pkg" ]] ||
        { printf 'error: no concrete Ubuntu 7.0 headers package is available\n' >&2; exit 1; }
fi
# modules-extra matches the frame dkms preflight: thunderbolt_frame_rxe
# imports ib_core/ib_umem symbols that Ubuntu ships there, and the preflight
# hard-fails when ib_uverbs.ko is absent for the target kernel (exactly what
# a minimal container is). Mirror what a real host carries.
modules_extra_pkg=""
if grep -qi '^ID=ubuntu' /etc/os-release; then
    modules_extra_pkg="${headers_pkg/linux-headers-/linux-modules-extra-}"
fi
apt-get install -y -qq --no-install-recommends \
	build-essential ca-certificates dkms file kmod "$headers_pkg" make \
	${modules_extra_pkg:+"$modules_extra_pkg"}

# Stage both packages' /usr/src trees WITHOUT running the postinst dkms
# autoinstall (the CI runner kernel has no matching headers; see
# tools/ci/distro-install.sh for the full rationale).
dpkg --unpack "$tbfix_deb"
dpkg --unpack "$frame_deb"

tbfix_src="$(find /usr/src -maxdepth 1 -type d -name 'thunderbolt-tbfix-*' -print -quit)"
frame_src="$(find /usr/src -maxdepth 1 -type d -name 'thunderbolt-frame-*' -print -quit)"
[[ -n "$tbfix_src" ]] || { printf 'error: thunderbolt-tbfix source not found under /usr/src\n' >&2; exit 1; }
[[ -n "$frame_src" ]] || { printf 'error: thunderbolt-frame source not found under /usr/src\n' >&2; exit 1; }
tbfix_ver="$(awk -F'"' '/^PACKAGE_VERSION=/ { print $2; exit }' "$tbfix_src/dkms.conf")"
frame_ver="$(awk -F'"' '/^PACKAGE_VERSION=/ { print $2; exit }' "$frame_src/dkms.conf")"
printf '==> tbfix source: %s (%s)\n' "$tbfix_src" "$tbfix_ver"
printf '==> frame source: %s (%s)\n' "$frame_src" "$frame_ver"

# Lockstep at the package level: the deb Depends pins (= version); the staged
# DKMS identities must agree with themselves too.
[[ "$tbfix_ver" == "$frame_ver" ]] || {
	printf 'error: version skew: tbfix %s vs frame %s\n' "$tbfix_ver" "$frame_ver" >&2
	exit 1
}

for required in dkms.conf Makefile kernel/frame kernel/rxe thunderbolt/thunderbolt_negotiation.h; do
	[[ -e "$frame_src/$required" ]] ||
		{ printf 'error: missing %s in %s\n' "$required" "$frame_src" >&2; exit 1; }
done

if [[ "${TBFIX_VERIFY_DKMS_BUILD:-0}" != "1" ]]; then
	printf '==> Package install/source verification OK\n'
	printf '==> Skipping DKMS build; set TBFIX_VERIFY_DKMS_BUILD=1 with matching 7.0 headers\n'
	exit 0
fi

kver="$(for d in /lib/modules/*/build; do [[ -e "$d" ]] && basename "$(dirname "$d")"; done | sort -V | tail -n 1)"
[[ -n "$kver" && -d "/lib/modules/$kver/build" ]] || { printf 'error: no kernel headers found\n' >&2; exit 1; }
printf '==> Kernel:     %s\n' "$kver"

# tbfix first: thunderbolt_frame needs its per-kernel Module.symvers and
# header shim.
for mod in thunderbolt-tbfix thunderbolt-frame; do
	ver="$tbfix_ver"
	[[ "$mod" == "thunderbolt-frame" ]] && ver="$frame_ver"
	dkms add -m "$mod" -v "$ver" || true
	if ! dkms build -m "$mod" -v "$ver" -k "$kver" --force; then
		cat "/var/lib/dkms/$mod/$ver/build/make.log" >&2 || true
		exit 1
	fi
done

for ko in thunderbolt_frame.ko thunderbolt_frame_rxe.ko; do
	built="$(find "/var/lib/dkms/thunderbolt-frame/$frame_ver" -name "$ko" -print -quit)"
	[[ -n "$built" ]] || { printf 'error: DKMS build did not produce %s\n' "$ko" >&2; exit 1; }
	file "$built"
	modinfo "$built" | sed -n '1,12p'
done

printf '==> thunderbolt-frame DKMS verification OK\n'
