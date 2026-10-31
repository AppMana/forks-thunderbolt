# rdma-core patches

The frame RDMA engine uses the stock rxe userspace ABI under a private driver
ID. rdma-core therefore needs an ID-aware provider even though the verbs ABI is
otherwise unchanged. These patches are build inputs; runtime packages contain
only the resulting provider shared object and driver hint.

The standalone package applies `0005` and installs:

```
/etc/libibverbs.d/tbrxe.driver
/usr/lib/<multiarch>/libibverbs/libtbrxe-rdmav<PABI>.so
```

`0004` instead extends the in-tree rxe provider. It is intended only for a full
rdma-core rebuild and is skipped by the standalone package builder. `0005`
builds the rxe provider a second time under the tbrxe identity, matching only
the frame engine's private driver ID. This lets it coexist with the
distribution's untouched rxe provider without either provider claiming the
other's devices.

Build the provider against the target distribution's rdma-core source so its
private ABI version matches the installed libibverbs:

```sh
OUT_DIR=dist tools/ci/distro-package-rdma.sh ubuntu
sudo dpkg -i dist/usb4-rdma-provider_*.deb
```

After changing `userspace/tbrxe`, regenerate `0005` from a clean upstream
baseline:

```sh
./packaging/regen-rdma-core-patches.sh
```

The packaging tests apply the patch series to a clean source tree, build it,
verify the registration symbol and driver hint, and inspect the provider's
private driver-ID match table.
