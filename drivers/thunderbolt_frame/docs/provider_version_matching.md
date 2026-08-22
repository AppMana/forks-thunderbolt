# usb4_rdma provider/kernel version matching (packaging invariant)

## The invariant

The **usb4_rdma userspace provider family** (`libusb4_rdma-rdmav<PABI>.so` +
`libtbrxe-rdmav<PABI>.so`, packaged as `usb4-rdma-provider`) and the kernel
engine (`thunderbolt_frame_rxe.ko`, packaged as `thunderbolt-frame-dkms`)
must roll together, and the provider must be built against the runtime
distro's rdma-core (jammy ships v39, noble v50).

`packaging/version` is the single source of truth for the provider deb
version (historically this was the legacy dkms.conf's `PACKAGE_VERSION`;
the kernel engine now rides the tbfix stream in `dkms/dkms.conf` instead).

## The engine provider

Since `0.2.70` the `usb4-rdma-provider` deb ships **`libtbrxe`**
(`libtbrxe-rdmav<PABI>.so` + `/etc/libibverbs.d/tbrxe.driver`), the provider
for `thunderbolt_frame_rxe.ko` (named `tbrxe.ko` before the 2.42 module
rename; the userspace provider keeps the tbrxe name). The engine speaks the
**stock rxe uverbs ABI** under the private `RDMA_DRIVER_USB4_RDMA` driver
id, so the failure mode differs from the retired legacy private-ABI engine:

- A container **without** `libtbrxe` (any provider `< 0.2.70`) sees the
  device in `/sys` but **no userspace provider binds it** — `ibv_devices`
  shows nothing, NCCL falls back or fails. Stock `librxe` can never bind it
  (driver-id match plus the ioctl header stamp both fail).
- Version skew *within* the engine/provider pair is not the legacy
  private-ABI hazard, but fleet doctrine stays: **ship the host's provider
  version in the container**.

The known consumers (`forks-vllm-consumer-nvidia-platforms/docker/Dockerfile`,
`forks-diffusion-pipe-prod/Dockerfile`) fetch the deb from the
`forks-thunderbolt` GitHub release tags (`v2.39` carried `0.2.70` for jammy
and noble) and record the baked version in
`/etc/appmana/usb4-rdma-provider-version` plus an
`ai.appmana.usb4_rdma.provider` image label so runtime preflights can compare
against the host without dpkg. Publish the current provider version to the
AppMana apt repo before any fleet host expects it.

## The failure mode when they skew

A stale provider against a newer kernel does **not** fail at device-open. It
fails silently on a *connected* QP under load:

```
ibv_post_recv -> rc=38 (ENOSYS)   # NOT a kernel bug (legacy-engine incident):
                                   # the legacy tbv_post_recv returned ENOMEM(12) on
                                   # a full RQ (proven by userspace/bench/rq_overflow_probe.c)
```

The application (NCCL via the tbchain mux) reports the unhelpful
`NCCL error: unhandled system error` at the first collective and aborts/hangs.
This cost a long debugging detour — the symptom points everywhere except the
real cause, which is purely packaging.

Diagnosed 2026-06-16: a 12-node NCCL all_reduce that aborted with provider
`0.2.3~jammy` (six versions behind the `0.2.9` kernel) ran **clean** the instant
the matching `0.2.9~jammy` provider was installed.

## Known-stale consumers (history; fixed 2026-08-19)

2026-06-16: the `harbor.appmana.com/appmana/vllm-ampere:*` images baked
`usb4-rdma-provider 0.2.3` against a `0.2.9` fleet kernel — the original
incident behind this document. 2026-08-03: the DiffusionPipe training image
was stale at `0.2.20` and its engine gate needed the host `libtbrxe`
bind-mounted because the image lacked it entirely. As of 2026-08-19 both
Dockerfiles pin `0.2.70` from the `v2.39` release; keep those args moving
with `packaging/version` when the provider version bumps.

## Build + bake the matching provider

```bash
# in an ubuntu:22.04 (jammy) container, from drivers/thunderbolt_frame/:
OUT_DIR=dist tools/ci/distro-package-rdma.sh ubuntu
#   -> dist/usb4-rdma-provider_<PACKAGE_VERSION>~jammy_amd64.deb
```

Bake it into the runtime image. The production pattern is the
`USB4_RDMA_PROVIDER_VERSION` / release-tag args in
`forks-vllm-consumer-nvidia-platforms/docker/Dockerfile` and
`forks-diffusion-pipe-prod/Dockerfile`: fetch the deb from the
`forks-thunderbolt` release, install it, assert both `.so` families and both
`.driver` files exist, and write the version to
`/etc/appmana/usb4-rdma-provider-version` (+ image label). The historical
`nccl-mux` packaging is superseded by NCCL rail routing.

## Assert it at runtime (fail fast, don't hang)

Bake a preflight that checks the provider version BEFORE launching NCCL, so a
skew dies immediately with a clear message instead of an opaque
"unhandled system error". The `tb-nccl-mux-bench` jobset shows the pattern:

```python
expect = os.environ["EXPECT_PROVIDER"]          # e.g. "0.2.70"
prov = subprocess.run("dpkg-query -W -f='${Version}' usb4-rdma-provider",
                      shell=True, capture_output=True, text=True).stdout
if not prov.split("~")[0].startswith(expect):
    sys.exit("FATAL: stale usb4-rdma-provider %s, expected %s" % (prov, expect))
```

Pin `EXPECT_PROVIDER` to `packaging/version` for the deployed kernel.
Images built after 2026-08-19 also expose the baked version at
`/etc/appmana/usb4-rdma-provider-version` for dpkg-free preflights.
```
