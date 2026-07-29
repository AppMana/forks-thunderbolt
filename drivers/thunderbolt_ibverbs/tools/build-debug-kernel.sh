#!/usr/bin/env bash
# Build a debug kernel for hunting the thunderbolt NHI tx ring stall.
#
# Produces an ADDITIONAL kernel next to the fleet's stock 6.17.0-40-generic.
# The stock kernel stays the grub default; the debug kernel is only ever
# entered via a one-shot `grub-reboot`, so any panic, watchdog reset or
# unexpected reboot lands back on stock with no human in the loop.
#
# Usage on the target host:
#   build-debug-kernel.sh fetch                 # get the exact -40 source
#   build-debug-kernel.sh build <flavour>       # dma | kcsan | kasan
#   build-debug-kernel.sh install <flavour>
#   build-debug-kernel.sh pin-stock             # make stock the saved default
#   build-debug-kernel.sh boot <flavour>        # one-shot boot into debug
#
# Base is the exact source of the running fleet kernel, Ubuntu
# linux-hwe-6.17 6.17.0-40.40~24.04.1 (upstream 6.17.13 + Ubuntu patches),
# with the running /boot/config-6.17.0-40-generic as the config base, so the
# only deltas are the debug options below.
set -euo pipefail

BASE="${BASE:-$HOME/kbuild}"
SRC="$BASE/src"
# The helper is often run while a debug kernel is active. `uname -r` would
# then pin that debug kernel as the saved fallback. Select the newest packaged
# generic kernel unless the caller explicitly supplies STOCK.
STOCK="${STOCK:-$(find /boot -maxdepth 1 -type f -name 'vmlinuz-*-generic' \
	-printf '%f\n' | sed 's/^vmlinuz-//' | sort -V | tail -1)}"
UPSTREAM_VER=6.17.13
DSC_VER='6.17.0-40.40~24.04.1'
JOBS="${JOBS:-$(nproc)}"

flavour_localversion() { echo "-tb$1"; }
flavour_kver() { echo "${UPSTREAM_VER}$(flavour_localversion "$1")"; }

fetch() {
	mkdir -p "$BASE"; cd "$BASE"
	# The .orig tarball is plain upstream 6.17 and is served fast from the
	# archive mirror. Only the small Ubuntu delta must come from Launchpad,
	# because -40 has already been superseded by -41 in the archive index.
	local LP="https://launchpad.net/ubuntu/+archive/primary/+sourcefiles/linux-hwe-6.17/$DSC_VER"
	local AR=http://us.archive.ubuntu.com/ubuntu/pool/main/l/linux-hwe-6.17
	[ -s linux-hwe-6.17_6.17.0.orig.tar.gz ] || curl -sSLf --max-time 900 -o linux-hwe-6.17_6.17.0.orig.tar.gz "$AR/linux-hwe-6.17_6.17.0.orig.tar.gz"
	for f in "linux-hwe-6.17_${DSC_VER}.dsc" "linux-hwe-6.17_${DSC_VER}.diff.gz"; do
		[ -s "$f" ] || curl -sSLf --max-time 300 -o "$f" "$LP/$f"
	done
	sudo apt-get install -y --no-install-recommends \
		build-essential bc bison flex libssl-dev libelf-dev libdw-dev \
		dwarves zstd rsync kmod cpio fakeroot dpkg-dev pkg-config
	rm -rf "$SRC"
	# --no-check: we deliberately skip the .dsc signature check because the
	# Ubuntu kernel signing key is not in the host keyring. Integrity comes
	# from the archive mirror over the size/hash check dpkg-source still does.
	dpkg-source --no-check -x "linux-hwe-6.17_${DSC_VER}.dsc" "$SRC"
}

# Options shared by every flavour: symbols good enough for crash/kgdb, the
# kdump and AER plumbing, and the cheap always-on allocator canaries.
common_config() {
	local O="$1" fl="$2"
	scripts/config --file "$O/.config" \
		--set-str LOCALVERSION "$(flavour_localversion "$fl")" \
		--disable LOCALVERSION_AUTO \
		--disable MODULE_SIG_ALL \
		--disable MODULE_SIG_FORCE \
		--set-str SYSTEM_TRUSTED_KEYS "" \
		--set-str SYSTEM_REVOCATION_KEYS "" \
		--disable SYSTEM_REVOCATION_LIST \
		--disable DEBUG_INFO_BTF \
		--disable DEBUG_INFO_NONE \
		--disable DEBUG_INFO_REDUCED \
		--enable DEBUG_KERNEL \
		--enable DEBUG_INFO_DWARF5 \
		--enable GDB_SCRIPTS \
		--enable FRAME_POINTER \
		--enable MAGIC_SYSRQ \
		--enable DEBUG_LIST \
		--enable KEXEC \
		--enable KEXEC_FILE \
		--enable CRASH_DUMP \
		--enable PROC_VMCORE \
		--enable PCIEAER \
		--enable KGDB \
		--enable KGDB_SERIAL_CONSOLE \
		--enable KGDB_KDB \
		--enable KDB_KEYBOARD \
		--disable STRICT_KERNEL_RWX \
		--disable RANDOMIZE_BASE \
		--enable KFENCE \
		--set-val KFENCE_SAMPLE_INTERVAL 100 \
		--set-val KFENCE_NUM_OBJECTS 1023
}

configure() {
	local fl="$1" O="$2"
	cp "/boot/config-$STOCK" "$O/.config"
	common_config "$O" "$fl"
	case "$fl" in
	dma)
		# Lowest-perturbation flavour. DMA_API_DEBUG is core code rather
		# than compiler instrumentation, and dma_debug_driver= scopes the
		# checking to one driver, so the timing shift is small enough that
		# the race is still expected to reproduce.
		scripts/config --file "$O/.config" --enable DMA_API_DEBUG
		;;
	kcsan)
		# KCSAN_WEAK_MEMORY is the reason this flavour exists: it models
		# store buffering and flags missing smp_wmb()/release barriers,
		# which is the closest automated analogue to the missing
		# dma_wmb() suspected in ring_write_descriptors(). Left disabled
		# at boot and armed via debugfs so boot does not eat the watchdog.
		scripts/config --file "$O/.config" \
			--enable DMA_API_DEBUG \
			--enable KCSAN \
			--enable KCSAN_STRICT \
			--enable KCSAN_WEAK_MEMORY \
			--disable KCSAN_EARLY_ENABLE \
			--disable KCSAN_IGNORE_ATOMICS \
			--enable KCSAN_INTERRUPT_WATCHER \
			--set-val KCSAN_REPORT_ONCE_IN_MS 0 \
			--set-val KCSAN_NUM_WATCHPOINTS 64 \
			--set-val KCSAN_SKIP_WATCH 4000
		;;
	kasan)
		# Memory-corruption flavour. Deliberately NOT combined with KCSAN:
		# the docs put generic KASAN at ~3x slowdown and KCSAN at ~5x, and
		# stacking them changes timing so much a race this narrow is
		# likely to stop reproducing at all.
		scripts/config --file "$O/.config" \
			--enable DMA_API_DEBUG \
			--enable KASAN \
			--enable KASAN_GENERIC \
			--enable KASAN_INLINE \
			--enable KASAN_VMALLOC \
			--enable STACKTRACE \
			--disable KFENCE
		;;
	*) echo "unknown flavour $fl" >&2; exit 1 ;;
	esac
	make -C "$SRC" O="$O" olddefconfig
}

build() {
	local fl="$1" O="$BASE/build-$1"
	mkdir -p "$O"; cd "$SRC"
	configure "$fl" "$O"
	make -C "$SRC" O="$O" -j"$JOBS"
}

# Flavours share one source tree via separate O= objtrees, but /lib/modules/$kv/build
# then points at an objtree that has only the generated headers. thunderbolt-tbfix's
# header shim reads $KDIR/include/linux/thunderbolt.h directly and fails on that.
# Union the source tree into the objtree with symlinks so $KDIR resolves both the
# generated and the source headers, which is what an in-tree build would look like.
union_srctree() {
	local O="$1" p b n=0
	for p in "$SRC"/* "$SRC"/include/* "$SRC"/arch/x86/include/*; do
		case "$p" in
		"$SRC"/include/*) b="include/$(basename "$p")" ;;
		"$SRC"/arch/x86/include/*) b="arch/x86/include/$(basename "$p")" ;;
		*) b="$(basename "$p")" ;;
		esac
		[ -e "$O/$b" ] || { ln -sfn "$p" "$O/$b"; n=$((n+1)); }
	done
	echo "unioned $n source entries into $O"
}

install_k() {
	local fl="$1" O="$BASE/build-$1" kv; kv="$(flavour_kver "$fl")"
	union_srctree "$O"
	# INSTALL_MOD_STRIP=1 is `strip --strip-debug`: it drops DWARF from the
	# installed modules but keeps the symbol table, so KCSAN/KASAN/oops
	# reports still resolve to symbol+offset. Without it a full Ubuntu
	# module set built with DWARF5 runs to tens of GB in /lib/modules and
	# makes initramfs generation crawl. Full DWARF is still in $O for
	# crash and gdb.
	sudo make -C "$SRC" O="$O" -j"$JOBS" INSTALL_MOD_STRIP=1 modules_install
	sudo make -C "$SRC" O="$O" install
	# DKMS needs a build tree for the new kernel. modules_install already
	# points /lib/modules/$kv/build at the O= dir, which redirects to $SRC.
	[ -e "/lib/modules/$kv/build" ] || sudo ln -sfn "$O" "/lib/modules/$kv/build"
	sudo update-initramfs -c -k "$kv"
	set_cmdline
	pin_stock
}

# The dma_debug entry pool must be far above the default 65536 for a workload
# that posts hundreds of thousands of frames. This goes on the global cmdline
# because dma_debug_entries is an early_param and there is no per-entry hook;
# it is simply ignored by the stock kernel, which has no DMA_API_DEBUG.
#
# dma_debug_driver= is deliberately NOT set. It scopes *reporting* to a single
# driver, and both thunderbolt and thunderbolt_ibverbs are in scope here, so
# filtering to one would hide the other. Add it by hand if noise is a problem.
set_cmdline() {
	local want='dma_debug_entries=262144'
	local effective

	effective="$(sh -c '. /etc/default/grub
		for f in /etc/default/grub.d/*.cfg; do
			[ -e "$f" ] && . "$f"
		done
		printf "%s %s\n" "$GRUB_CMDLINE_LINUX_DEFAULT" "$GRUB_CMDLINE_LINUX"')"
	case " $effective " in
	*" $want "*) ;;
	*)
		echo "$want is missing from the composed GRUB drop-ins" >&2
		echo "declare it as host_kernel_cmdline in appmana-management inventory.yaml" >&2
		exit 1
		;;
	esac
}

# Pin the SAVED grub default to the stock kernel by its generated menu id, so
# version ordering can never promote a debug kernel to default.
stock_entry_id() {
	local uuid; uuid="$(sudo grub-probe --target=fs_uuid /boot 2>/dev/null || sudo grub-probe --target=fs_uuid /)"
	echo "gnulinux-advanced-$uuid>gnulinux-$STOCK-advanced-$uuid"
}
debug_entry_id() {
	local uuid kv; kv="$(flavour_kver "$1")"
	uuid="$(sudo grub-probe --target=fs_uuid /boot 2>/dev/null || sudo grub-probe --target=fs_uuid /)"
	echo "gnulinux-advanced-$uuid>gnulinux-$kv-advanced-$uuid"
}

pin_stock() {
	printf '%s\n' \
		'# Managed by build-debug-kernel.sh; the debug kernel is never the persistent default.' \
		'GRUB_DEFAULT=saved' |
		sudo tee /etc/default/grub.d/05-debug-kernel-default.cfg >/dev/null
	sudo update-grub
	sudo grub-set-default "$(stock_entry_id)"
	echo "saved default is now: $(sudo grub-editenv list | grep saved_entry || true)"
}

boot_debug() {
	local fl="$1"
	# One-shot only. next_entry is consumed on the next boot, so a panic or
	# a watchdog reset returns to stock without anyone having to intervene.
	sudo grub-reboot "$(debug_entry_id "$fl")"
	sudo grub-editenv list
}

case "${1:?usage: $0 fetch|build|install|pin-stock|boot [flavour]}" in
fetch) fetch ;;
build) build "${2:?flavour}" ;;
install) install_k "${2:?flavour}" ;;
pin-stock) pin_stock ;;
boot) boot_debug "${2:?flavour}" ;;
*) echo "unknown command $1" >&2; exit 1 ;;
esac
