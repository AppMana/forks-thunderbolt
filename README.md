# forks-thunderbolt

AppMana fork of the Linux kernel Thunderbolt drivers and our out-of-tree
`thunderbolt_ibverbs` (usb4_rdma RDMA) transport, in **one repo** so the three
host-to-host Thunderbolt stacks share a single negotiation header instead of
hand-synced copies:

- **`thunderbolt`** — the core XDomain/USB4 driver.
- **`thunderbolt_net`** — IP-over-Thunderbolt (`tbnet`).
- **`thunderbolt_ibverbs`** — the legacy usb4_rdma RDMA transport + userspace provider.
- **`tbframe` + `tbrxe`** — the rxe-derived successor RDMA engine (lossless frame
  service + IB engine); see "Two RDMA engines" below.
- **`rdma_rxe`** — AppMana's soft-RoCE fallback override for `rxe_lan`.

All three negotiate the same XDomain connection and shared the same soft-reconnect
bug; the fix lives once in `drivers/thunderbolt/thunderbolt_negotiation.h`
(generation gate + handshake re-arm + the `TB_XNEG_*` installable macros), with
KUnit, run via `drivers/thunderbolt_ibverbs/tools/run-kunit.sh` (`kunit.py` on an
overlaid v6.17 tree, x86_64 qemu; the fleet/host kernels lack `CONFIG_KUNIT`).

```
upstream  = git://git.kernel.org/pub/scm/linux/kernel/git/westeri/thunderbolt.git
origin    = git@github.com:AppMana/forks-thunderbolt.git
branch    = pub/tbfix-v6.17
base      = v6.17 + 3 backports already in linux-hwe-6.17
```

## What's here

```
drivers/thunderbolt/            full subsystem source (patched) + the shared
                                thunderbolt_negotiation.h (single source of truth)
drivers/net/thunderbolt/        tbnet driver source (patched)
drivers/infiniband/sw/rxe/      RXE source from Ubuntu HWE 6.17 with AppMana
                                `rdma link del` protection for `rxe_lan`
drivers/thunderbolt_ibverbs/    legacy usb4_rdma RDMA driver, userspace providers,
                                dkms, tests (merged from the old thunderbolt-ibverbs
                                repo), plus the successor engine:
  kernel/tbframe/               session + lossless frame service on XDomain rings
  kernel/tbrxe/                 rxe-derived IB engine over tbframe
  userspace/tbrxe/              libtbrxe provider source (rdma-core patch 0005)
  docs/tbframe-tbrxe-wire-spec.md   normative wire/contract spec for the new stack
dkms/                           DKMS scaffolding for thunderbolt-tbfix-dkms
  tbrxe/                        DKMS scaffolding for thunderbolt-tbrxe-dkms
                                (tbframe.ko + tbrxe.ko in one package)
packaging/debian/           Debian package metadata (thunderbolt-tbfix-dkms
                            unsuffixed; *-tbrxe for thunderbolt-tbrxe-dkms)
scripts/
  oot-build.sh              fast iteration: stage at ~/src/tb-oot, build, hot-swap
  export-dkms-payload.sh    write byte-identical DKMS bundle into the appmana repo
tools/ci/
  distro-package.sh         build thunderbolt-tbfix-dkms_<version>_all.deb
  distro-package-tbrxe.sh   build thunderbolt-tbrxe-dkms_<version>_all.deb
  distro-install.sh         verify the tbfix .deb can build through DKMS
  distro-install-tbrxe.sh   verify the tbrxe .deb can build through DKMS
tests/
  run-smoke.sh              60-s NCCL hostnet sweep on the 3-node chain
  run-durability.sh         192 GiB allreduce reproducer (~30 min, the wedge gate)
```

## How to use

Read `docs/thunderbolt_fix.md` in the parent `appmana` repo. It covers:

1. Building OOT for fast iteration (`scripts/oot-build.sh --swap`).
2. Building the DKMS payload for fleet deploy (`scripts/export-dkms-payload.sh`).
3. Running the smoke + durability tests.
4. Deploying via Ansible (`playbook_worker.yaml`).
5. Reverting to in-tree drivers.
6. Rebasing on a newer kernel when the fleet bumps.
7. Preparing upstream topic patches for manual review.

## Two RDMA engines: legacy `thunderbolt_ibverbs` and `tbrxe` + `tbframe`

The repo now carries **two** host-to-host RDMA engines. Both publish ib_devices
named `usb4_rdma*`, so consumers (NCCL, perftest) see the same device names
either way.

| | legacy `thunderbolt_ibverbs` | `tbrxe` + `tbframe` (successor) |
|---|---|---|
| Kernel modules | `thunderbolt_ibverbs.ko` (dkms, `0.2.6x`) | `tbframe.ko` + `tbrxe.ko` (`thunderbolt-tbrxe-dkms`, tbfix version stream) |
| Verbs ABI | private (provider must match version exactly) | stock rxe uverbs ABI under a private driver id |
| Userspace provider | `libusb4_rdma` | `libtbrxe` (rdma-core patch 0005) |
| Provider package | `usb4-rdma-provider` (any) | `usb4-rdma-provider >= 0.2.70` |
| Wire protocol | `proto/native_data.h` (being retired) | wire v3 per `docs/tbframe-tbrxe-wire-spec.md` |

**Status (2026-08-19).** The tbrxe stack has passed a real two-rank
pipeline-parallel training gate over NCCL `NET/IB` on `usb4_rdma0`
(60/60 steps, loss decreasing, clean counters; evidence in
`drivers/thunderbolt_ibverbs/traces/20260819-m4-diffusionpipe-023-025/`).

### tbrxe's driver id and the provider requirement

`tbrxe.ko` registers under its own RDMA driver id,
`RDMA_DRIVER_USB4_RDMA` (`0x55534234`, "USB4"), not `RDMA_DRIVER_RXE`
(commit `7815b8f`: with the stock id, `rmmod rdma_rxe` swept live tbrxe
devices off the node). rdma-core matches providers by driver id and stamps
the compiled-in id into every uverbs ioctl header, so **stock `librxe`
cannot bind a tbrxe device**; a `usb4-rdma-provider` deb >= 0.2.70 is
required in every userspace that touches the device — hosts *and*
containers. The 0.2.70 deb ships both provider families
(`libusb4_rdma-rdmav<PABI>.so`, `libtbrxe-rdmav<PABI>.so`), both
`/etc/libibverbs.d/*.driver` files, and the udev rail tooling (stable
`tbr-<peer>` naming + deterministic per-link ULA /64 assignment via
`tbv-rdma-addr`). `tbrxe.ko` and the provider deb must roll together. See
`drivers/thunderbolt_ibverbs/packaging/rdma-core-patches/README.md`.

### Build coupling and the deploy procedure

tbfix core, tbframe and tbrxe are **lockstep-built**: tbframe links against
the tbfix core's `Module.symvers` (private `tb_*` exports) and the generated
`<linux/thunderbolt.h>` header shim (`dkms/tbfix-gen-thunderbolt-header.sh`
— building source-blind against the stock header causes an NX-execute
panic on first inbound traffic); tbrxe links against tbframe's
`Module.symvers`. Never mix builds from different revisions.

The packaging enforces that coupling. `thunderbolt-tbrxe-dkms` ships BOTH
modules in one DKMS package (`dkms/tbrxe/`): its build makes tbframe first
and feeds tbframe's freshly written `Module.symvers` to tbrxe within the
same pass, resolves the tbfix core symbols from the per-kernel
`/var/lib/dkms/thunderbolt-tbfix/...` table, and hard-fails (instead of
building source-blind) when the tbfix header-shim script or symvers is
missing. At the package level the version rides the tbfix stream and the
deb pins `Depends: thunderbolt-tbfix-dkms (= <version>)`; it also
`Conflicts/Replaces: thunderbolt-ibverbs-dkms` — a node runs ONE RDMA
engine, and both engines claim the `usb4_rdma*` device names.

The package installs **no autoload** (no `modules-load.d`). Loading is a
deliberate act after boot:

```sh
modprobe tbrxe       # depmod dependency pulls in tbframe first
```

(Before the deb existed, the two `.ko` were built by hand from
`drivers/thunderbolt_ibverbs/kernel/{tbframe,tbrxe}/` and insmod'd from a
staged directory; that development-era procedure is retired.)

Deploys go via **reboot, one node at a time**, with bidirectional link
verification (`ib_send_lat` both ways or counter deltas) before any
workload. Never reload the modules under active RDMA traffic — that is the
silicon-wedge class. Even idle ring teardown/re-create without a reboot has
wedged an NHI TX ring (`driver_head` advances, `nhi_tail` stuck at 0)
roughly every other event in testing (2026-08-18); the BYE/READY_ACK
spec work narrows the windows but reboot remains the deploy path.

### Wire and behavior summary (normative spec: `docs/tbframe-tbrxe-wire-spec.md`)

- **Wire v3**: verbs report `IB_MTU_4096` while the engine fragments at the
  4012-byte payload ceiling (4096 frame − 80 header budget − 4 ICRC, aligned
  down to 4). Mixed wire versions refuse the session at HELLO.
- **Mode A admission** (default, universal): sender-side static window
  bounded by the peer's `rx_ring_entries` (2048 rings ⇒ 1984-frame data
  window after the 64-slot control reserve). No credit messages exist.
- **BYE/BYE_ACK** orderly-teardown quiesce (ThunderboltIP LOGOUT analog)
  protects the peer's router egress from streaming into a torn-down path.
- **Interrupt moderation off by default** on tbframe data rings
  (`data_ring_throttle_ns=0`, commit `18c990c`): the NHI's 128 us MSI-X
  moderation was the whole latency gap vs the legacy engine.
- **Loss model**: CRC frame loss at ~2-4e-6/frame is normal on this fabric
  and recovered by the rxe PSN machinery; retransmits use exponential
  backoff (`retransmit_base_ms` default 10 ms, doubling per consecutive
  timeout, capped at the verbs timeout, reset by any peer response). Frames
  are never silently lost while the link is UP; loss windows are bracketed
  by link_down/link_up.

### Measured performance (two-node Maple Ridge x1 test pair, 2026-08-18/19)

- `ib_send_lat` 64 B: ~15 us typical (best of campaign 12.89 us; legacy
  Strix record 8.3 us).
- `ib_write_bw` 1 MiB: 17.3 Gb/s unidirectional single-QP; 23.6-23.8 Gb/s
  aggregate bidirectional at 32 QPs.
- NCCL (2-rank, Ring/Simple): all_reduce busbw 1.71-1.77 GB/s, all_gather
  1.63-1.71 GB/s at large sizes; no degraded iterations after the backoff
  work.
- The bandwidth ceiling is **frame rate**, not bytes: the NHI moves ~0.5M
  frames/s per direction (~540k measured) regardless of QP count or CPU;
  bytes-per-frame (the MTU-4096 deviation) is the lever.

Evidence: `drivers/thunderbolt_ibverbs/traces/20260818-*/RESULTS.txt` and
`20260819-m4-diffusionpipe-023-025/RESULTS.txt`.

### Lane bonding verdict (2026-08-19)

**Negotiated (XDP) lane bonding cannot work on Maple Ridge hosts.** The
resident ICM firmware intercepts the link-state XDP requests and NAKs them
(`ERROR_NOT_SUPPORTED`) before the peer OS ever sees them — measured by
packet trace; driver version and scheduling are irrelevant
(`traces/20260818-bonding-rearm-023-025/RESULTS.txt`). The bounded re-arm
(tbfix 2.38, `78a7bbd`) and route-checked XDP error matching (`b042f53`)
are correct and stay, but x2 requires **direct bonding** (both ends
arm lane 1 + bonding bit themselves), which is designed but not yet
enabled. Strix-class integrated hosts bond natively — the committed
`bench/results/strix-2p-noiommu-2x40g/` results are 2 lanes @ 20 Gb/s per
cable (40 Gb/s aggregate) without any of this.

### Component inventory for a working vLLM deployment (example)

Every lib/component in the path, whether it is forked, and how it reaches a
node or container. Environment policy (the NCCL env below) belongs to the
deployment's orchestration layer, not to images.

| Component | Forked? | Source | Bundled / installed how |
|---|---|---|---|
| Linux kernel (6.17 HWE) | no | Ubuntu | host package manager |
| `thunderbolt` core + `thunderbolt_net` (tbfix) | **yes** | this repo (`drivers/thunderbolt`, `drivers/net/thunderbolt`) | host: `thunderbolt-tbfix-dkms` deb (release assets). Core bumps are cold-boot-only |
| `tbframe.ko` (frame/session layer) | new | this repo (`kernel/tbframe`) | host: `thunderbolt-tbrxe-dkms` deb (lockstep with the core: symvers + header shim); loaded via `modprobe tbrxe`, never autoloaded |
| `tbrxe.ko` (IB engine) | fork of in-tree `rxe` | this repo (`kernel/tbrxe`) | same `thunderbolt-tbrxe-dkms` deb, built lockstep in the same DKMS pass |
| `rdma_rxe` (ethernet soft-RoCE fallback device) | ~stock (one fail-fast patch) | kernel / this repo | module in the tbfix deb; device created via `rdma link add ... type rxe` |
| rdma-core / `libibverbs` | no | distro (noble = v50, PABI 34) | host + container, stock |
| `usb4-rdma-provider` deb (`libusb4_rdma` + `libtbrxe` + `.driver` files + udev ULA addressing) | patches, not a fork (`packaging/rdma-core-patches/`) | this repo | host: deb >= 0.2.70. container: same deb baked in, or injected from the host at runtime (PABI must match the container's rdma-core) |
| NCCL | **yes** | rail-routing fork (2.30.7 base) | baked into the image; required for correct multi-HCA rail selection |
| NCCL net plugin | n/a — disabled | — | `NCCL_NET_PLUGIN=none` via deployment env (external plugins crash on PCI-less HCAs and bypass the fork's routing) |
| NCCL env | policy | deployment | injected by the orchestration layer; see the env list below |
| vLLM | **yes** | serving fork | the serving image |
| perftest / nccl-tests | no | upstream | validation tooling only |

### Consuming the stack from NCCL (either engine)

Use the rail-routing NCCL fork (2.30.7 base): it advertises all device GIDs
in the listen handle and selects the `usb4_rdma` rail that shares a /64 with
the peer, with `rxe_lan` as the ordered fallback. Stock NCCL cannot do this
on multi-HCA hosts (each rail reaches a different neighbor; a wrong pick is
unroutable, not just slow).

Environment to set (per process/deployment):

```sh
NCCL_IB_DISABLE=0
NCCL_IB_HCA=usb4_rdma,rxe_lan     # ordered preference; ==usb4_rdma0 pins exactly
NCCL_IB_ADDR_FAMILY=AF_INET6      # rail GIDs are IPv6 ULAs
NCCL_IB_SUBNET_AWARE_ROUTING='prefer_hca[usb4_rdma\d*,rxe_lan\d*]'
NCCL_ALGO=Ring                    # linear topologies: adjacent rails only;
NCCL_PROTO=Simple                 # tree graphs create unreachable edges
NCCL_NET_PLUGIN=none              # see below
NCCL_SOCKET_IFNAME=<bootstrap-if> # TCP bootstrap; data rides the rails
NCCL_NET_GDR_LEVEL=0
NCCL_IB_MERGE_NICS=0
NCCL_NET_MERGE_LEVEL=LOC
```

`NCCL_NET_PLUGIN=none` is mandatory wherever an external NCCL net plugin is
installed (e.g. HPC-X in NGC images): the plugin loads in preference to
NCCL's builtin IB transport, derives topology from each device's PCI path
and segfaults on these PCI-less HCAs (`nccl_p2p_ib_pci_path`) — and even a
non-crashing plugin would bypass the fork's rail routing entirely.

Containers additionally need the matching `usb4-rdma-provider` (>= 0.2.70
for tbrxe hosts; exactly the host's version for legacy hosts — see
`drivers/thunderbolt_ibverbs/docs/provider_version_matching.md`),
`/dev/infiniband`, `IPC_LOCK`/unlimited memlock, and adequate `/dev/shm`.

## Who owns the link: `usb4_rdma` vs `thunderbolt_net`

Both drivers can be loaded at once, and both advertise their XDomain services
(`tbverbs` -> `thunderbolt_ibverbs`, `network` -> `thunderbolt_net`). But **one
driver owns the link's DMA data path at a time**, and which one is a runtime
switch — no `rmmod`, no reboot:

```sh
# hand the link to thunderbolt_net (IP over Thunderbolt: tb-ch* netdevs)
echo tbnet > /sys/module/thunderbolt_ibverbs/parameters/link_owner

# hand it back to usb4_rdma (RDMA: usb4_rdma* ib_devices)  [default]
echo rdma  > /sys/module/thunderbolt_ibverbs/parameters/link_owner

# who owns it right now
grep link_owner /sys/kernel/debug/thunderbolt_ibverbs/summary
```

`rdma` is the default; set it at boot with
`options thunderbolt_ibverbs link_owner=tbnet` in `/etc/modprobe.d/`.

- **`link_owner=tbnet`** — `thunderbolt_ibverbs` unpublishes its `usb4_rdma`
  ib_devices and disables its DMA tunnels. It does *not* tear down rings or
  negotiation, so the switch back is cheap. `thunderbolt_net` then establishes
  its tunnel and the `tb-ch*` netdevs start passing packets.
- **`link_owner=rdma`** — each rail re-runs its HELLO negotiation, re-enables its
  tunnel and republishes the ib_device. `ib_send_lat` works again.

The switch is idempotent and applied on a bounded workqueue (no RTNL, no hotplug
notifiers, no ICM interaction), so it cannot wedge the chain the way a module
reload does.

### Why `thunderbolt_net` used to be a zombie

Before **tbfix 2.13 / tbv 0.2.36**, `thunderbolt_net` on a chain node looked
healthy — netdev `up`, `carrier=1`, correct IPs, routes resolving — and passed
**zero packets**. That was *not* a resource conflict with `usb4_rdma`: the two
use separate NHI rings and separate XDomain hopids, and both hop entries are
programmed. Reading the hop entries back off the routers showed the real
mechanism: tbnet's inbound lane-adapter entries were **deactivated**
(`enable=0`) while tbv's entries on the same adapters were `enable=1` and
carrying traffic.

tbnet got stuck that way because its ThunderboltIP session is **one-shot**:
`login_work` and `connected_work` both early-return once carrier is on, so once
both ends latch "login complete" nothing ever re-LOGINs — and this fleet
demonstrably loses the hotplug edges upstream relies on to tear the session down
(a neighbour rebooting can produce *no* thunderbolt event on the surviving node).
`thunderbolt_ibverbs` survives the identical events because its HELLO
negotiation is level-triggered.

The fix (in `thunderbolt_net`, using the stock spec LOGIN — the negotiation is
not reimplemented) is a **level-triggered session verify**: while carrier is on,
re-check that the DMA tunnel behind the session is still alive
(`tb_xdomain_paths_active()` reads every hop entry back from the routers), and
if it died, tear the session down and re-run a normal LOGIN.

```sh
# how often to re-check the tunnel behind an established session (ms); 0 disables
cat /sys/module/thunderbolt_net/parameters/session_verify_ms   # default 5000
```

A healthy link logs nothing; recovery logs
`ThunderboltIP session lost its DMA tunnel; tearing down and re-logging in`.

## Install on Ubuntu

### From the AppMana apt repository

Once `AppMana/apt` has published the release, install with:

```bash
curl -fsSL https://appmana.github.io/apt/appmana-archive-keyring.gpg \
  | sudo gpg --dearmor -o /usr/share/keyrings/appmana-archive-keyring.gpg

echo 'deb [arch=amd64 signed-by=/usr/share/keyrings/appmana-archive-keyring.gpg] https://appmana.github.io/apt noble main' \
  | sudo tee /etc/apt/sources.list.d/appmana.list

sudo apt update
sudo apt install thunderbolt-tbfix-dkms

# tbframe/tbrxe engine (optional; version-locked to thunderbolt-tbfix-dkms,
# conflicts with the legacy thunderbolt-ibverbs-dkms):
sudo apt install thunderbolt-tbrxe-dkms usb4-rdma-provider
```

### From GitHub Releases

Download `thunderbolt-tbfix-dkms_<version>_all.deb` from:

```text
https://github.com/AppMana/thunderbolt-tbfix/releases
```

Then install it:

```bash
sudo apt install build-essential dkms kmod make "linux-headers-$(uname -r)"
sudo apt install ./thunderbolt-tbfix-dkms_<version>_all.deb
dkms status -m thunderbolt-tbfix
```

The package stages source under `/usr/src/thunderbolt-tbfix-<version>` and
runs `dkms autoinstall`. It intentionally does not reload `thunderbolt` or
`thunderbolt_net`; reload ordering remains owned by the deployment's
maintenance procedure.

`thunderbolt-tbrxe-dkms_<version>_all.deb` (same release, same version)
installs the tbframe/tbrxe engine the same way: source under
`/usr/src/thunderbolt-tbrxe-<version>`, both modules built through DKMS in
one pass. It never loads the modules — after the deploy reboot, run
`modprobe tbrxe` and verify the link bidirectionally before any workload.

The split-package build also emits `rdma-rxe-appmana_<version>_all.deb`.
That package installs an override `rdma_rxe.ko` which does not advertise
`IBK_ALLOW_USER_UNREG`. On AppMana, `rxe_lan` is a live NCCL fallback rail;
`rdma link del rxe_lan` should fail fast instead of unregistering the device
while Ray/NCCL still owns QPs.

Verify the installed module path:

```bash
modinfo thunderbolt | sed -n '1,8p'
modinfo thunderbolt_net | sed -n '1,8p'
```

`filename` should point under `/lib/modules/<kernel>/updates/`, not the
stock `/kernel/drivers/...` tree.

## Build a Debian package

Build the DKMS `.deb`s locally:

```bash
tools/ci/distro-package.sh ubuntu        # thunderbolt-tbfix-dkms
tools/ci/distro-package-tbrxe.sh ubuntu  # thunderbolt-tbrxe-dkms (tbframe+tbrxe)
```

The artifacts are written to `dist/thunderbolt-tbfix-dkms_<version>_all.deb`
and `dist/thunderbolt-tbrxe-dkms_<version>_all.deb` (with `.sha256` files;
both carry the same version — the tbrxe deb depends on the exact tbfix
version). Tags matching `v*` publish the `.deb`s and their `.sha256` files
to GitHub Releases in `AppMana/thunderbolt-tbfix`. The public apt
repository in `AppMana/apt` consumes those release assets.

## Development process

Install build and test dependencies on an Ubuntu development host:

```bash
sudo apt update
sudo apt install build-essential dkms git kmod "linux-headers-$(uname -r)" \
  linux-tools-common linux-tools-generic trace-cmd
```

Fast one-host edit/build loop:

```bash
scripts/oot-build.sh
scripts/oot-build.sh --install
```

`--install` writes the freshly built modules to
`/lib/modules/$(uname -r)/updates/` and runs `depmod`. To live-swap on a
test host:

```bash
scripts/oot-build.sh --swap
```

Do not run `--swap` on a production chain node unless the Thunderbolt link can
be interrupted. It unloads and reloads `thunderbolt_net` and `thunderbolt`.

DKMS/package loop:

```bash
tools/ci/distro-package.sh ubuntu
sudo apt install ./dist/thunderbolt-tbfix-dkms_<version>_all.deb
sudo dkms build -m thunderbolt-tbfix -v <version> -k "$(uname -r)" --force
sudo dkms install -m thunderbolt-tbfix -v <version> -k "$(uname -r)" --force
```

Container verification, without touching host modules:

```bash
docker run --rm -v "$PWD:/work" -w /work ubuntu:24.04 \
  bash tools/ci/distro-package.sh ubuntu

docker run --rm -v "$PWD:/work" -w /work ubuntu:24.04 \
  bash tools/ci/distro-install.sh 'dist/thunderbolt-tbfix-dkms_*.deb'
```

The container install check verifies the `.deb` metadata and `/usr/src`
staging. It does not compile the module by default because this tbfix branch is
for the fleet's 6.17 Thunderbolt tree, while stock Ubuntu 24.04 containers
install 6.8 headers. To compile in a matching kernel-header environment:

```bash
TBFIX_VERIFY_DKMS_BUILD=1 tools/ci/distro-install.sh \
  'dist/thunderbolt-tbfix-dkms_*.deb'
```

Functional tests:

```bash
tests/run-smoke.sh
tests/run-durability.sh
```

`run-smoke.sh` is the quick gate. `run-durability.sh` is the wedge gate and
should complete the 192 GiB allreduce target before fleet rollout.

Tracing and diagnostics:

```bash
sudo mount -t tracefs nodev /sys/kernel/tracing 2>/dev/null || true
sudo trace-cmd list | grep -Ei 'thunderbolt|tbnet|nhi|irq'
sudo trace-cmd record -e thunderbolt:* -e napi:* -e irq:* -- sleep 30
sudo trace-cmd report | less
```

Useful live checks:

```bash
sudo dmesg -T | grep -Ei 'thunderbolt|tb-ch|DMA paths|login|host found' | tail -100
ls -l /sys/bus/thunderbolt/devices
for d in /sys/bus/thunderbolt/devices/*; do
  [ -e "$d" ] || continue
  echo "== $d =="
  for f in device_name unique_id rx_speed tx_speed rx_lanes tx_lanes authorized; do
    [ -r "$d/$f" ] && echo "$f=$(cat "$d/$f")"
  done
done
ip -br link | grep -E 'tb-ch|thunderbolt'
```

For NHI interrupt-mask debugging, inspect Thunderbolt debugfs if available:

```bash
sudo find /sys/kernel/debug/thunderbolt -maxdepth 3 -type f -print 2>/dev/null
```

Capture these before and after smoke/durability runs when changing
`drivers/thunderbolt/nhi.c`, `drivers/thunderbolt/path.c`, or
`drivers/net/thunderbolt/main.c`.

## Branches

- `pub/tbfix-v6.17` — deployed DKMS branch. Includes the Ubuntu HWE
  backport alignment, Thunderbolt networking/ring reliability work,
  DKMS packaging, and ICM hotplug diagnostics.
- `pub/tbfix-v6.17-hotplug` — storage-hotplug split branch. Carries
  only the `drivers/thunderbolt/icm.c` hotplug work on top of the DKMS
  packaging point; no `drivers/net/thunderbolt` changes.
- `master` — local mirror of `upstream/master` (Mika Westerberg's tree).

Do not submit the fleet branch upstream. For upstream work, create a
fresh topic branch from `upstream/master` and apply only the minimal
subsystem-specific change. Thunderbolt networking changes touch netdev;
storage hotplug changes should not.

## Operational caveats

- **An ICM wedge is cleared only by removing power.** Symptom: `thunderbolt
  0000:xx:00.0: failed to send driver ready to ICM`, `probe ... error -110`,
  and no `usb4_rdma*` device. Measured 2026-07-09 on Maple Ridge: warm reboot,
  PCI secondary-bus reset, D3cold via runtime PM (no `_PR3` on these boards),
  and the WMI `force_power` toggle **all fail**. `systemctl poweroff` to S5
  followed by power-on clears it every time. Arm Wake-on-LAN first
  (`ethtool -s <if> wol g`) or the node needs a physical power button press —
  some boards ignore WoL from S5 regardless.
- **Never live-reload the modules on a chain node.** `rmmod`/`modprobe` of
  `thunderbolt_ibverbs` under traffic wedges the ICM. The deploy path is
  `dpkg -i` then `rmmod` the module *before* rebooting (a warm reboot with
  active NHI rings leaves the ICM mid-transaction) — see
  `appmana-management/.../scripts/tb-chain-reboot-cover.sh`, which drains,
  unloads, reboots a pairwise non-adjacent set, and verifies negotiation.
- **A rail's peers only re-negotiate after its neighbour also rolls.** After
  rebooting one half of the chain, `data-ready-rails=0` on those nodes is
  expected until their neighbours reboot too; do not chase it.
- **Loss recovery is deadline-armed since 0.2.25.** Before that, every lost
  frame recovered on a ~100 ms grid regardless of `tbv_retransmit_base_ms`,
  because the backoff deadline was only a threshold checked when the QP timeout
  work happened to run, and every arming site scheduled that work at
  `min3(ack_timeout, 1000ms, TBV_READ_RESP_RETRY_MS=100ms)`. Symptom to watch
  for in `/sys/kernel/debug/thunderbolt_ibverbs/summary`:
  `data_rx_ack_match_over_64ms` climbing with `data_wr_retransmit`, and
  `data_rx_duplicate_ack` close to the retransmit count (spurious resends).
- **`data_rx_bad_header` is not necessarily wire corruption.** Until 0.2.26 the
  RX path never inspected `frame->flags`, so frames the NHI had already marked
  `RING_DESC_CRC_ERROR` / `RING_DESC_BUFFER_OVERRUN` were parsed anyway. The
  `data_rx_crc_error` / `data_rx_overrun` counters now separate hardware-flagged
  corruption from software framing bugs. Note `enum ring_desc_flags` aliases TX
  and RX meanings on the same bits (`0x1` ISOCH/CRC_ERROR, `0x4` POSTED/
  BUFFER_OVERRUN) — the check is only valid on an RX completion.
- **Hardware E2E flow control is NOT a throughput win.** The `c5ec614` commit
  message reports ~28 Gb/s duplex with E2E vs ~3 Gb/s on software credits; that
  came from a synthetic bandwidth sweep and does not reproduce on real NCCL
  message shapes, where the two are about the same. E2E remains off for the
  native backend: it buys nothing measurable and carries the Maple Ridge
  ring-teardown lockup and AMD TX-completion wedge history. Do not re-derive a
  policy change from that commit message alone.

## Why patches not a submodule

`forks-*` siblings of the appmana monorepo are independent git repos by
convention (see `forks-sglang`, `forks-vllm-ampere`). Ansible deploys
from a flat in-repo copy of the DKMS payload at
`appmana-management/src/appmana_management/files/thunderbolt_net/tbfix-dkms/`.

The `scripts/export-dkms-payload.sh` script in this fork rewrites that
copy from `HEAD`. If the fork advances, run the script and commit the
appmana-side change; `diff -r` between fork export and appmana copy must
be empty before deploy.

## License

Linux kernel sources are GPL-2.0-only; the DKMS/scripts overlay matches.
