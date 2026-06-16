# usb4_rdma provider/kernel version matching (packaging invariant)

## The invariant

The **usb4_rdma userspace provider** (`libusb4_rdma-rdmav34.so`, packaged as
`usb4-rdma-provider`) and the **kernel module** (`thunderbolt_ibverbs`, packaged
as `thunderbolt-ibverbs-dkms`) share a private verb ABI. They **must be the same
version**, and the provider must be built against the runtime distro's rdma-core
(jammy ships v39, noble v50; the provider source adapts via `PACKAGE_VERSION`
guards in `userspace/usb4_rdma/CMakeLists.txt`).

`PACKAGE_VERSION` in `dkms.conf` is the single source of truth for both.

## The failure mode when they skew

A stale provider against a newer kernel does **not** fail at device-open. It
fails silently on a *connected* QP under load:

```
ibv_post_recv -> rc=38 (ENOSYS)   # NOT a kernel bug: ENOSYS appears nowhere in
                                   # kernel/; tbv_post_recv returns ENOMEM(12) on
                                   # a full RQ (proven by userspace/bench/rq_overflow_probe.c)
```

The application (NCCL via the tbchain mux) reports the unhelpful
`NCCL error: unhandled system error` at the first collective and aborts/hangs.
This cost a long debugging detour — the symptom points everywhere except the
real cause, which is purely packaging.

Diagnosed 2026-06-16: a 12-node NCCL all_reduce that aborted with provider
`0.2.3~jammy` (six versions behind the `0.2.9` kernel) ran **clean** the instant
the matching `0.2.9~jammy` provider was installed.

## Known-stale consumers (FIX THESE)

The `harbor.appmana.com/appmana/vllm-ampere:*` images (including the **DSV4**
serving image `ibverbs-cpufanout-*` used by
`clusters/appmana-cluster-03/inference/lws-vllm-deepseek-v4.yaml`) bake
**`usb4-rdma-provider 0.2.3`** while the fleet kernel is **0.2.9**. They must be
rebuilt with the current provider. The fleet *hosts* also still carry `0.2.3` —
bump them too.

## Build + bake the matching provider

```bash
# in an ubuntu:22.04 (jammy) container, from the thunderbolt-ibverbs checkout:
OUT_DIR=dist tools/ci/distro-package-rdma.sh ubuntu
#   -> dist/usb4-rdma-provider_<PACKAGE_VERSION>~jammy_amd64.deb
```

Bake it into the runtime image (see `nccl-mux/packaging/Dockerfile`, which builds
the mux on the same jammy base and overlays the provider `.deb` + the plugin, so
nothing — glibc floor, provider ABI, plugin path — can drift at runtime). For
production, do the same in `forks-vllm-ampere/docker/Dockerfile.ampere-runtime`.

## Assert it at runtime (fail fast, don't hang)

Bake a preflight that checks the provider version BEFORE launching NCCL, so a
skew dies immediately with a clear message instead of an opaque
"unhandled system error". The `tb-nccl-mux-bench` jobset shows the pattern:

```python
prov = subprocess.run("dpkg-query -W -f='${Version}' usb4-rdma-provider",
                      shell=True, capture_output=True, text=True).stdout
if not prov.split("~")[0].startswith(os.environ.get("EXPECT_PROVIDER", "0.2.9")):
    sys.exit("FATAL: stale usb4-rdma-provider %s, expected 0.2.9" % prov)
```

Pin `EXPECT_PROVIDER` to `dkms.conf`'s `PACKAGE_VERSION` for the deployed kernel.
```
