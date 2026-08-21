#!/usr/bin/env bash
# Verify that a built tbfix package cannot fail installation by attempting to
# compile its Linux-7.0 source against retained kernels from another ABI line.

set -euo pipefail

deb="${1:?usage: test-tbfix-package.sh <thunderbolt-tbfix-dkms.deb>}"
work_dir="$(mktemp -d)"
trap 'rm -rf "$work_dir"' EXIT

version="$(dpkg-deb -f "$deb" Version)"
dpkg-deb -x "$deb" "$work_dir/root"
dpkg-deb -e "$deb" "$work_dir/control"

dkms_conf="$work_dir/root/usr/src/thunderbolt-tbfix-$version/dkms.conf"
postinst="$work_dir/control/postinst"

grep -Fqx 'BUILD_EXCLUSIVE_KERNEL="^7[.]0[.]"' "$dkms_conf"
grep -Fq 'case "$kernelver" in' "$postinst"
grep -Fq '7.0.*) ;;' "$postinst"
grep -Fq 'skipping unsupported kernel $kernelver' "$postinst"

printf 'tbfix package kernel compatibility policy: OK (version %s)\n' "$version"
