#!/bin/sh
# Suppress the in-tree (distro) thunderbolt_net.ko so only the tbfix DKMS build
# is resolvable at boot.
#
# Why: the DKMS thunderbolt_net.ko installs to updates/dkms/ (higher modprobe
# priority), but the distro copy stays in kernel/drivers/net/thunderbolt/ and
# initramfs-tools (MODULES=most) globs the kernel/ tree, baking the DISTRO
# module into the initramfs. That distro module was built against UPSTREAM core
# CRCs, so at early boot it loads against our patched thunderbolt core and
# floods the log with 16 lines of
#   thunderbolt_net: disagrees about version of symbol tb_xdomain_alloc_out_hopid
#   thunderbolt_net: Unknown symbol tb_ring_start (err -22)
# before our updates/ copy loads and actually runs (refcnt 0 on the failed
# distro attempt). The -22 is cosmetic -- our module is what runs -- but the
# flood buries real boot/crash evidence (it, plus CREDDBG, reduced appmana-019's
# boot log to six thunderbolt lines).
#
# Rather than delete a distro-shipped file (a linux-modules upgrade would
# restore it) or blocklist the module name (that would also block OUR copy from
# auto-loading), stash the distro files OUT of the module tree so depmod and
# initramfs cannot see them, and restore them on package removal. Reversible,
# idempotent, per-kernel (dkms POST_INSTALL fires on every kernel dkms builds
# for, including new kernels).
#
# Usage: tbfix-net-suppress.sh disable|restore [kernelver]
# In a DKMS POST_INSTALL the kernel version comes from the exported $kernelver.
set -eu

action="${1:-}"
kver="${kernelver:-${2:-}}"
[ -n "$kver" ] || kver="$(uname -r)"

moddir="/lib/modules/$kver/kernel/drivers/net/thunderbolt"
stash="/var/lib/thunderbolt-tbfix/disabled-net/$kver"

refresh() {
	depmod -a "$kver" 2>/dev/null || true
	if command -v update-initramfs >/dev/null 2>&1; then
		update-initramfs -u -k "$kver" 2>/dev/null || true
	fi
}

case "$action" in
disable)
	[ -d "$moddir" ] || exit 0
	moved=0
	for ko in "$moddir"/thunderbolt_net.ko*; do
		[ -e "$ko" ] || continue	# no-nullglob guard
		mkdir -p "$stash"
		mv -f "$ko" "$stash/"
		moved=1
	done
	[ "$moved" = 1 ] && refresh
	;;
restore)
	[ -d "$stash" ] || exit 0
	mkdir -p "$moddir"
	moved=0
	for ko in "$stash"/thunderbolt_net.ko*; do
		[ -e "$ko" ] || continue
		mv -f "$ko" "$moddir/"
		moved=1
	done
	rmdir "$stash" 2>/dev/null || true
	[ "$moved" = 1 ] && refresh
	;;
*)
	echo "usage: $0 disable|restore [kernelver]" >&2
	exit 2
	;;
esac

exit 0
