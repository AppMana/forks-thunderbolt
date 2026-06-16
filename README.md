# forks-thunderbolt

AppMana fork of the Linux kernel Thunderbolt drivers and our out-of-tree
`thunderbolt_ibverbs` (usb4_rdma RDMA) transport, in **one repo** so the three
host-to-host Thunderbolt stacks share a single negotiation header instead of
hand-synced copies:

- **`thunderbolt`** — the core XDomain/USB4 driver.
- **`thunderbolt_net`** — IP-over-Thunderbolt (`tbnet`).
- **`thunderbolt_ibverbs`** — the usb4_rdma RDMA transport + userspace provider.

All three negotiate the same XDomain connection and shared the same soft-reconnect
bug; the fix lives once in `drivers/thunderbolt/thunderbolt_negotiation.h`
(generation gate + handshake re-arm + the `TB_XNEG_*` installable macros), with
KUnit + plain-`cc` userspace coverage (fleet kernels lack `CONFIG_KUNIT`).

```
upstream  = git://git.kernel.org/pub/scm/linux/kernel/git/westeri/thunderbolt.git
origin    = git@github.com:AppMana/forks-thunderbolt.git
branch    = pub/tbfix-v6.17    (deployed on the AppMana fleet)
base      = v6.17 + 3 backports already in linux-hwe-6.17
```

## What's here

```
drivers/thunderbolt/            full subsystem source (patched) + the shared
                                thunderbolt_negotiation.h (single source of truth)
drivers/net/thunderbolt/        tbnet driver source (patched)
drivers/thunderbolt_ibverbs/    usb4_rdma RDMA driver, userspace provider, dkms,
                                tests (merged from the old thunderbolt-ibverbs repo)
dkms/                           DKMS scaffolding (dkms.conf, Makefile)
packaging/debian/           Debian package metadata for thunderbolt-tbfix-dkms
scripts/
  oot-build.sh              fast iteration: stage at ~/src/tb-oot, build, hot-swap
  export-dkms-payload.sh    write byte-identical DKMS bundle into the appmana repo
tools/ci/
  distro-package.sh         build thunderbolt-tbfix-dkms_<version>_all.deb
  distro-install.sh         verify the .deb can build through DKMS
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
`thunderbolt_net`; fleet reload ordering remains owned by Ansible or an
explicit maintenance command.

Verify the installed module path:

```bash
modinfo thunderbolt | sed -n '1,8p'
modinfo thunderbolt_net | sed -n '1,8p'
```

`filename` should point under `/lib/modules/<kernel>/updates/`, not the
stock `/kernel/drivers/...` tree.

## Build a Debian package

Build the DKMS `.deb` locally:

```bash
tools/ci/distro-package.sh ubuntu
```

The artifact is written to `dist/thunderbolt-tbfix-dkms_<version>_all.deb`.
Tags matching `v*` publish the `.deb` and its `.sha256` file to GitHub
Releases in `AppMana/thunderbolt-tbfix`. The public apt repository in
`AppMana/apt` consumes those release assets.

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
