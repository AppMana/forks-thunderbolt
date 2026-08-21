# drivers/thunderbolt_frame

The AppMana Thunderbolt/USB4 host-to-host RDMA engine family and its
userspace, packaging, tooling and evidence. Descended from the
Hellas `thunderbolt-ibverbs` research driver (standalone repo
`AppMana/forks-thunderbolt-ibverbs`, historical); the legacy
`thunderbolt_ibverbs` engine itself has been removed — the surviving stack
was renamed from `tbframe`/`tbrxe` to the family convention with the 2.42
release.

## Layout

```
frame/          thunderbolt_frame.ko — session + lossless frame service on
                XDomain DMA rings. Internal symbol namespace stays tbframe_*;
                the on-wire XDomain service key stays "tbframe" (wire
                contract — pre-rename peers interoperate).
  tests/        KUnit suites (run via tools/run-kunit.sh)
rxe/            thunderbolt_frame_rxe.ko — rxe-derived IB engine over the
                frame service. Registers ib_devices named usb4_rdma* under
                the private RDMA_DRIVER_USB4_RDMA id (0x55534234, "USB4").
  tests/        KUnit suites (run via rxe/tests/run-kunit.sh)
userspace/
  usb4_rdma/    libusb4_rdma provider source (rdma-core patches 0001-0003)
  tbrxe/        libtbrxe provider source (patch 0005) — the provider that
                binds the engine's driver id; provider names are unchanged
  bench/        generic RDMA verbs micro-benchmarks and probes
packaging/
  version       version stream for the userspace debs (usb4-rdma-provider,
                the reproduction-tools deb)
  rdma-core-patches/   the provider patch series + README
  udev/         60-usb4-rdma-net.rules + tbv-rdma-ifname/tbv-rdma-addr:
                stable tbr-<peer> / tbr-lo<route> naming and deterministic
                per-link ULA /64 assignment for the u4r* rail netdevs
  debian/, rpm/, arch/   usb4-rdma-provider + tools package metadata
tools/          run-kunit.sh, tbv-hang-repro, NHI register dumpers,
                latency timelines, lock-graph, CI packaging scripts
docs/           frame-rxe-wire-spec.md (normative wire/contract spec),
                provider_version_matching.md, debugging methodology docs
bench/, traces/, tests/repro/   measurement and incident evidence
kernel-workflow/  upstream-thunderbolt patch pipeline for the core fork
```

## Building

Both modules build out-of-tree against the running kernel's headers, linked
against the thunderbolt-tbfix core (see the per-module Makefiles for the
symvers/header-shim discipline):

```sh
make -C frame            # thunderbolt_frame.ko
make -C rxe              # thunderbolt_frame_rxe.ko (reads ../frame/Module.symvers)
```

The shipped package is `thunderbolt-frame-dkms` (both modules in one DKMS
pass, version-locked to `thunderbolt-tbfix-dkms`); it is built by
`tools/ci/distro-package-frame.sh` at the repo root. The userspace provider
deb is built by `tools/ci/distro-package-rdma.sh` here.

## Tests

```sh
tools/run-kunit.sh           # core thunderbolt + tbnet + thunderbolt_frame suites
rxe/tests/run-kunit.sh       # thunderbolt_frame_rxe engine suites
```

Both stand up a throwaway KUnit kernel (kunit.py, x86_64 qemu) over an
overlaid v6.17 tree; no host modules are touched.

Engine doctrine, deploy rules, measured performance and the NCCL consumption
recipe live in the top-level [`README.md`](../../README.md).
