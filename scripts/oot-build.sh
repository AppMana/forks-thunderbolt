#!/usr/bin/env bash
# Build the patched Thunderbolt modules through the same staged source,
# generated public header, and Makefile used by the DKMS package.
#
# A live rmmod/modprobe cycle is deliberately not offered: Maple/Titan Ridge
# ICM firmware can remain running but stop answering after the host driver
# unloads, and only a board cold-power cycle recovers it. --install writes the
# modules for the next boot and refreshes depmod; it does not touch live state.

set -euo pipefail

REPO_ROOT="$(git -C "$(dirname "$0")" rev-parse --show-toplevel)"
KREL="$(uname -r)"
KDIR="/lib/modules/$KREL/build"
OOT="${TBFIX_OOT_DIR:-/tmp/thunderbolt-tbfix-oot-${UID}}"

if [ ! -d "$KDIR" ]; then
	echo "kernel build dir missing: $KDIR (install linux-headers-$KREL)" >&2
	exit 1
fi

mode="build"
case "${1:-}" in
	--install) mode="install" ;;
	--build|"") mode="build" ;;
	--swap)
		echo "--swap was removed: live ICM module replacement can require a cold power cycle" >&2
		echo "use --install, then reboot through the normal GRUB drop-in workflow" >&2
		exit 2
		;;
	-h|--help)
		echo "usage: $0 [--build|--install]"
		echo "  --build    stage and build with the canonical DKMS workflow"
		echo "  --install  also install for the next boot and run depmod"
		exit 0
		;;
	*)
		echo "unknown arg: $1" >&2
		exit 1
		;;
esac

"$REPO_ROOT/scripts/export-dkms-payload.sh" \
	--target "$OOT" --worktree
make -C "$OOT" KDIR="$KDIR" TBFIX_SOURCE_DIR="$OOT" -j"$(nproc)"

if [ "$mode" = "build" ]; then
	echo "OOT build complete at $OOT"
	ls -la \
		"$OOT/drivers/thunderbolt/thunderbolt.ko" \
		"$OOT/drivers/net/thunderbolt/thunderbolt_net.ko"
	exit 0
fi

sudo install -D -m 0644 "$OOT/drivers/thunderbolt/thunderbolt.ko" \
	"/lib/modules/$KREL/updates/dkms/thunderbolt.ko"
sudo install -D -m 0644 "$OOT/drivers/net/thunderbolt/thunderbolt_net.ko" \
	"/lib/modules/$KREL/updates/dkms/thunderbolt_net.ko"
sudo depmod -a

echo "Installed for $KREL under /lib/modules/$KREL/updates/dkms/."
echo "Reboot through the normal GRUB drop-in workflow to load it."
