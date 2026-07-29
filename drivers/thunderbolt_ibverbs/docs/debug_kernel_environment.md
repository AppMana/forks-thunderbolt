# debug kernel environment on appmana-025 and appmana-023

Companion to [dma_ring_stall_debug_methodology.md](dma_ring_stall_debug_methodology.md),
which explains *why* these options were chosen. This document is the operational
half: what is installed, how to build it, how to boot it, and how to get back.

**Scope.** Only appmana-025 and appmana-023 (chain positions 7 and 8, adjacent).
No other host has been touched.

## safety design

The single most important property of this setup: **the stock kernel is the saved
grub default and the debug kernel is only ever entered as a one-shot.**

`build-debug-kernel.sh` sets `GRUB_DEFAULT=saved` and pins the saved entry to the
stock `6.17.0-40-generic` menu id. Booting a debug kernel uses `grub-reboot`, which
writes `next_entry` into the grub environment and grub consumes it on the next boot.

The consequence is what matters: a panic, a hard lockup, a watchdog reset or any
unplanned reboot lands back on the stock kernel with nobody in the loop. There is no
state in which the machine repeatedly boots a broken debug kernel.

Both hosts also have Secure Boot **disabled** and kernel lockdown `[none]`, verified,
so a locally-built unsigned kernel boots and kgdb is not blocked.

## what is built

Base is the exact source of the running fleet kernel: Ubuntu `linux-hwe-6.17`
`6.17.0-40.40~24.04.1`, which is upstream **6.17.13** plus Ubuntu patches. Note the
package is versioned `6.17.0-40` but the tree is 6.17.13.

`-40` has already been superseded by `-41` in the archive index, so the build script
takes the `.orig` tarball from the archive mirror (it is plain upstream 6.17 and
identical for both) and only the small `.dsc`/`.diff.gz` delta from Launchpad.

The config base is the running host's own `/boot/config-6.17.0-40-generic`, so the
only deltas are the debug options. Three flavours are defined.

### shared by all flavours

Symbols and plumbing for kgdb/kdump, plus the cheap always-on checks:

```
CONFIG_DEBUG_INFO_DWARF5=y      CONFIG_GDB_SCRIPTS=y
CONFIG_DEBUG_INFO_REDUCED=n     CONFIG_DEBUG_INFO_BTF=n
CONFIG_FRAME_POINTER=y          CONFIG_MAGIC_SYSRQ=y
CONFIG_DEBUG_LIST=y
CONFIG_KEXEC=y                  CONFIG_KEXEC_FILE=y
CONFIG_CRASH_DUMP=y             CONFIG_PROC_VMCORE=y
CONFIG_PCIEAER=y
CONFIG_KGDB=y                   CONFIG_KGDB_SERIAL_CONSOLE=y
CONFIG_KGDB_KDB=y               CONFIG_KDB_KEYBOARD=y
CONFIG_STRICT_KERNEL_RWX=n      # or kgdb software breakpoints do not work
CONFIG_RANDOMIZE_BASE=n         # KASLR confuses gdb symbol resolution
CONFIG_KFENCE=y
CONFIG_KFENCE_SAMPLE_INTERVAL=100
CONFIG_KFENCE_NUM_OBJECTS=1023  # (1023+1)*2*4KiB = 8 MiB pool
```

Module signing is disabled and `SYSTEM_TRUSTED_KEYS`/`SYSTEM_REVOCATION_KEYS` are
cleared, because the Ubuntu signing keys are not available locally. `DEBUG_INFO_BTF`
is off to avoid a pahole dependency and cut build time; nothing here needs BTF.

Deliberately **not** enabled: `CONFIG_PROVE_LOCKING`. That ground is already covered
by `.lockdep-run/` and `docs/lock_deadlock_verification.md`, and lockdep is one more
timing perturbation for no new information about this bug.

### flavour `dma` — the primary, lowest-perturbation kernel

```
CONFIG_DMA_API_DEBUG=y
```

That is the only delta on top of the shared set. `DMA_API_DEBUG` is core code rather
than compiler instrumentation, and `dma_debug_driver=` scopes it to one driver, so
the timing shift is small enough that a ~1-in-450k race still has a real chance of
reproducing. **This is the flavour to use first**, because a debug kernel in which
the bug no longer reproduces is worthless.

### flavour `kcsan` — data races and missing barriers

```
CONFIG_DMA_API_DEBUG=y
CONFIG_KCSAN=y                  CONFIG_KCSAN_STRICT=y
CONFIG_KCSAN_WEAK_MEMORY=y      CONFIG_KCSAN_EARLY_ENABLE=n
CONFIG_KCSAN_IGNORE_ATOMICS=n   CONFIG_KCSAN_INTERRUPT_WATCHER=y
CONFIG_KCSAN_REPORT_ONCE_IN_MS=0
CONFIG_KCSAN_NUM_WATCHPOINTS=64 CONFIG_KCSAN_SKIP_WATCH=4000
```

`KCSAN_WEAK_MEMORY` is the reason this flavour exists — it models store buffering and
flags missing `smp_wmb()`/release barriers. `REPORT_ONCE_IN_MS=0` removes rate
limiting so no report is dropped.

`EARLY_ENABLE=n` is deliberate: KCSAN costs about 5x, and letting it run during boot
risks tripping the 120 s watchdog before anyone has typed a command. Arm it manually
immediately before the run:

```bash
echo on | sudo tee /sys/kernel/debug/kcsan
```

### flavour `kasan` — memory corruption

```
CONFIG_DMA_API_DEBUG=y
CONFIG_KASAN=y  CONFIG_KASAN_GENERIC=y  CONFIG_KASAN_INLINE=y
CONFIG_KASAN_VMALLOC=y  CONFIG_STACKTRACE=y  CONFIG_KFENCE=n
```

Lowest priority — see the methodology document for why corruption is unlikely here.
`KASAN_INLINE` over `OUTLINE` because it is roughly twice as fast for a larger binary,
which matters when trying to keep a timing-dependent bug alive.

**KASAN and KCSAN are separate flavours on purpose.** ~3x on top of ~5x would almost
certainly stop the race reproducing at all, and the two findings are independently
actionable.

## build

Everything lives in `~/kbuild` on the host. Nothing is built in `/tmp`.

```bash
scp drivers/thunderbolt_ibverbs/tools/build-debug-kernel.sh administrator@appmana-025.i.appmana.com:~/
ssh administrator@appmana-025.i.appmana.com

~/build-debug-kernel.sh fetch          # source + build deps, once
~/build-debug-kernel.sh build dma      # ~25 min on 32 cores
~/build-debug-kernel.sh install dma    # modules_install + install + initramfs + pin stock
```

Build deps beyond the obvious: `libdw-dev` is required (Ubuntu's config enables
`gendwarfksyms`, which needs `dwarf.h`) and its absence is a confusing early failure
in `scripts`.

Flavours share one source tree via separate `O=` directories
(`~/kbuild/build-dma`, `~/kbuild/build-kcsan`, `~/kbuild/build-kasan`), so a second
flavour does not re-fetch or re-patch anything.

## boot into it

```bash
~/build-debug-kernel.sh boot dma       # one-shot; sets grub next_entry
sudo systemctl reboot                  # only after graceful-drain, see below
```

Verify after it comes up:

```bash
uname -r                               # expect 6.17.13-tbdma
cat /proc/cmdline
```

## get back to stock

```bash
sudo grub-editenv - unset next_entry   # see the caveat below
sudo systemctl reboot
```

Always confirm the environment before and after:

```bash
sudo grub-editenv list
# saved_entry should name 6.17.0-40-generic
# next_entry present means the NEXT boot goes to the debug kernel
```

**Caveat, observed on these hosts.** `next_entry` is documented as one-shot and grub
is supposed to clear it on use, but on both appmana-025 and appmana-023 it survived
the boot it triggered. Do not assume the one-shot cleared itself — check
`grub-editenv list` and unset it explicitly. Both hosts were left with `next_entry`
unset and `saved_entry` pinned to stock, so an unattended panic or watchdog reset
returns them to `6.17.0-40-generic`.

The consequence is that booting the debug kernel is an explicit act every time:

```bash
~/build-debug-kernel.sh boot dma && sudo systemctl reboot
```

To remove a debug kernel entirely:

```bash
sudo rm -rf /boot/vmlinuz-6.17.13-tbdma /boot/initrd.img-6.17.13-tbdma \
            /boot/System.map-6.17.13-tbdma /boot/config-6.17.13-tbdma \
            /lib/modules/6.17.13-tbdma
sudo update-grub
```

## cmdline drift found on both hosts

Worth knowing because it silently degrades any measurement taken after the next
reboot. `/etc/default/grub` on both hosts had been reset to
`GRUB_CMDLINE_LINUX_DEFAULT="quiet splash"` on 2026-07-24, dropping the fleet's
Thunderbolt tuning. The running kernels still had the full set only because they
had booted *before* that change:

```
nmi_watchdog=1 hugepagesz=2M hugepages=1024 pcie_ports=native
pci=assign-busses,hpbussize=0x33,realloc,hpmmiosize=128M,hpmmioprefsize=16G
pcie_aspm=off
```

`playbook_worker.yaml` (lines 450 and 510-511) is the owner of these and will
restore them on its next run, so this was pre-existing drift, not caused by this
work. It was restored by hand on both hosts so the debug kernel reproduces the
fleet's actual PCI/Thunderbolt resource layout — without it the comparison to
production is not sound. Verify with `cat /proc/cmdline` after any reboot.

Note also that `/etc/default/grub.d/90-iommu-off.cfg` adds `amd_iommu=off`, so the
IOMMU is disabled on these hosts. Any IOMMU hypothesis is moot here, and the absence
of `AMD-Vi` events in dmesg is because the IOMMU is off, not because it is healthy.

## node lifecycle — mandatory

Both hosts were taken out of service through the sanctioned flow:

```bash
./appmana-management/src/appmana_management/scripts/graceful-drain.sh appmana-025
./appmana-management/src/appmana_management/scripts/graceful-uncordon.sh appmana-025
```

Run sequentially, never both in parallel.

**They are currently dedicated debug hosts and are deliberately left cordoned**
(`Ready,SchedulingDisabled`) with zero workloads. Do not uncordon them as a courtesy
— that is the owner's call. While they are in this state you may reboot, wedge or
panic them freely; nothing else is using the rails, and the DSV4 inference workload
is scaled to 0 via GitOps so the whole chain is uncontended.

If they are ever returned to general service, drain/uncordon through those scripts
as normal, and note that the `nvidia` DKMS module does not build against the debug
kernel, so a host serving GPU work must be booted back onto stock first.

## watchdog

Both hosts run an armed SP5100 watchdog at `RuntimeWatchdogSec=120`
(`/etc/systemd/system.conf.d/10-watchdog.conf`).

**The build itself trips it too, which is not obvious.** appmana-023 was hard-reset
by the SP5100 during `make -j32`:

```
x86/amd: Previous system reset reason [0x02000800]: hardware watchdog timer expired
```

A `-j32` kernel build on a 32-core node starves systemd past the 120 s deadline. So
disable the watchdog **before building**, not just before running a slow kernel, or
pass `JOBS=16`. appmana-025 survived the same build; do not rely on that.

A KCSAN or KASAN kernel is slow enough to trip that spuriously. Before running either
of those flavours, or any build, on **both** hosts:

```bash
sudo mkdir -p /etc/systemd/system.conf.d
printf '[Manager]\nRuntimeWatchdogSec=0\n' | sudo tee /etc/systemd/system.conf.d/20-tbdbg-watchdog-off.conf
sudo systemctl daemon-reexec
```

Restore afterwards — this is not optional, and it is the last step of the work:

```bash
sudo rm -f /etc/systemd/system.conf.d/20-tbdbg-watchdog-off.conf
sudo systemctl daemon-reexec
grep -r RuntimeWatchdogSec /etc/systemd/system.conf.d/    # expect 120
```

The `dma` flavour is not slow enough to need this.

## dkms — build tbfix BEFORE ibverbs

Both out-of-tree modules must be rebuilt against the debug kernel, **and the order
matters**:

```bash
sudo dkms install thunderbolt-tbfix/2.13    -k 6.17.13-tbdma   # FIRST
sudo dkms install thunderbolt-ibverbs/0.2.42 -k 6.17.13-tbdma   # SECOND
dkms status | grep 6.17.13-tbdma
```

Historical sources lacked a DKMS dependency edge, so `dkms autoinstall` built
alphabetically, did ibverbs first, and produced a module that failed to load:

```
thunderbolt_ibverbs: disagrees about version of symbol tb_ring_poll
thunderbolt_ibverbs: Unknown symbol tb_ring_poll (err -22)
thunderbolt_ibverbs: Unknown symbol tb_xdomain_disable_paths (err -22)
```

`thunderbolt_ibverbs` links against symbols exported by tbfix's replacement
`thunderbolt.ko`. Built before tbfix has been installed, it records the CRCs of the
*in-tree* `thunderbolt.ko` and then refuses to load against tbfix's. The symptom is
no `usb4_rdma*` rails and only two thunderbolt modules loaded. Current source
declares `BUILD_DEPENDS[0]="thunderbolt-tbfix"` so autoinstall orders the
modules. To recover an older already-registered source tree, uninstall, unbuild
and reinstall ibverbs after tbfix:

```bash
sudo dkms uninstall thunderbolt-ibverbs/0.2.42 -k 6.17.13-tbdma
sudo dkms unbuild   thunderbolt-ibverbs/0.2.42 -k 6.17.13-tbdma
sudo dkms install   thunderbolt-ibverbs/0.2.42 -k 6.17.13-tbdma
sudo modprobe thunderbolt_ibverbs
```

`nvidia` fails to build against this kernel and that is expected and harmless here —
these hosts are drained, and the GPU driver is not involved in the RDMA path. It is
why `make install` exits non-zero via `/etc/kernel/postinst.d/dkms`; the kernel
itself is installed correctly by that point, so just run the remaining steps
(`update-initramfs`, `set_cmdline`, `pin-stock`) by hand.

`thunderbolt-tbfix` supplies the forked `drivers/thunderbolt` (including the `nhi.c`
under investigation) and `thunderbolt-ibverbs` supplies the RDMA driver. Both must be
present or the rails will not appear.

For the `kcsan` and `kasan` flavours this step is **not optional and not cosmetic**:
those sanitizers are compiler instrumentation, and kbuild applies the flags to
out-of-tree modules too. A module built against a stock kernel and loaded on a KCSAN
kernel is simply not instrumented, and the run will be quiet for the wrong reason.

## running the reproducer under the debug kernel

`install` already appends `dma_debug_entries=262144` to
`GRUB_CMDLINE_LINUX_DEFAULT`, because the default pool of 65536 is far too small
for a workload posting hundreds of thousands of frames. It is an `early_param` with
no per-entry hook, so it goes on the global cmdline and is simply ignored by the
stock kernel, which has no `DMA_API_DEBUG`.

`dma_debug_driver=` is deliberately not set. It scopes *reporting* to one driver,
and both `thunderbolt` and `thunderbolt_ibverbs` are in scope here, so filtering to
either would hide the other. Add it by hand only if the noise from unrelated drivers
becomes a problem.

```bash
# report every error, not just the first
echo 1 | sudo tee /sys/kernel/debug/dma-api/all_errors

# kcsan flavour only
echo on | sudo tee /sys/kernel/debug/kcsan

cd forks-thunderbolt
TBV_QPS=32 TBV_SIZE=262144 TBV_ROUNDS=3 \
  ./drivers/thunderbolt_ibverbs/tools/tbv-hang-repro.sh appmana-025 appmana-023
```

Then collect, on both hosts:

```bash
sudo dmesg | grep -iE 'DMA-API|KCSAN|KASAN|BUG:|WARNING:|AER'
sudo cat /sys/kernel/debug/dma-api/error_count
sudo cat /sys/kernel/debug/kcsan
```

## reference state

Verified on both hosts before any change:

| property | appmana-025 | appmana-023 |
|---|---|---|
| kernel | 6.17.0-40-generic | 6.17.0-40-generic |
| cores / RAM | 32 / 60 GB | 32 / 60 GB |
| free disk on `/` | 709 G | 398 G |
| Secure Boot | disabled | disabled |
| lockdown | `[none]` | `[none]` |
| rails | `usb4_rdma5`, `usb4_rdma15` | `usb4_rdma5`, `usb4_rdma15` |
| uverbs holders | none | none |
| `crashkernel=` | present | present |
| PCIe AER errors | none | none |
| IOMMU faults | none | none |

`crashkernel=2G-4G:320M,4G-32G:512M,32G-64G:1024M,64G-128G:2048M,128G-:4096M` is
already on the fleet cmdline; at 60 GB these hosts reserve 1024M.
