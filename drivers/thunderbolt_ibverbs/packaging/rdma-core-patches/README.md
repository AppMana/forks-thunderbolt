# rdma-core patches

The userspace `usb4_rdma` provider is an out-of-tree libibverbs provider that
plugs into rdma-core via the standard `providers/` mechanism. Rather than
fork rdma-core or vendor its internal headers, we ship **three minimal
patches** that any rdma-core source tree can apply to produce a build that
includes our provider alongside the upstream ones.

These are build inputs, not patches installed on fleet kernels or on the
runtime `rdma-core` packages. The normal distro packaging script applies them
to a temporary, version-matched rdma-core source tree, builds the plugin, and
puts only these two files in the standalone `usb4-rdma-provider` package:

```
/etc/libibverbs.d/usb4_rdma.driver
/usr/lib/<multiarch>/libibverbs/libusb4_rdma-rdmav<PABI>.so
```

```
0001-providers-usb4_rdma-add-USB4-soft-RDMA-provider.patch
   Adds providers/usb4_rdma/ (the provider source files)

0002-CMakeLists.txt-build-the-usb4_rdma-provider.patch
   Adds `add_subdirectory(providers/usb4_rdma)` to the top-level
   CMakeLists.txt so the provider is built and installed.

0003-libibverbs-verbs.h-declare-verbs_provider_usb4_rdma.patch
   Declares the provider's exported registration symbol for static-provider
   builds.
```

The patches are auto-generated from the source in `userspace/usb4_rdma/`
via `git format-patch`, and apply cleanly against upstream rdma-core ≥ v62.

## How each consumer applies them

### Nix (this repo)

Already wired up — `rdma-core-usb4` in `flake.nix` applies all three files on
top of `pkgs.rdma-core`.

### Debian / Ubuntu

Build the standalone provider package against the distro's own rdma-core
source and PABI:

```sh
OUT_DIR=dist tools/ci/distro-package-rdma.sh ubuntu
sudo dpkg -i dist/usb4-rdma-provider_*.deb
```

Rebuilding all of the distro's rdma-core packages with the provider embedded
is also possible, but is not the fleet deployment path:

```sh
apt-get source rdma-core
cd rdma-core-*
cp .../packaging/rdma-core-patches/000*.patch debian/patches/
ls debian/patches/000*.patch >> debian/patches/series
dpkg-buildpackage -b -uc -us
sudo dpkg -i ../librdmacm1_*.deb ../libibverbs1_*.deb ../ibverbs-providers_*.deb
```

### Arch (AUR)

In a PKGBUILD that derives from `extra/rdma-core`, add the patches to
`source=(...)` and a `prepare()` function:

```bash
source=(
  "git+https://github.com/linux-rdma/rdma-core.git#tag=v62.0"
  "0001-providers-usb4_rdma-add-USB4-soft-RDMA-provider.patch"
  "0002-CMakeLists.txt-build-the-usb4_rdma-provider.patch"
  "0003-libibverbs-verbs.h-declare-verbs_provider_usb4_rdma.patch"
)
prepare() {
  cd "$srcdir/rdma-core"
  patch -p1 < ../0001-providers-usb4_rdma-add-USB4-soft-RDMA-provider.patch
  patch -p1 < ../0002-CMakeLists.txt-build-the-usb4_rdma-provider.patch
  patch -p1 < ../0003-libibverbs-verbs.h-declare-verbs_provider_usb4_rdma.patch
}
```

### Fedora / RHEL

Add to `rdma-core.spec`:

```spec
Patch1000: 0001-providers-usb4_rdma-add-USB4-soft-RDMA-provider.patch
Patch1001: 0002-CMakeLists.txt-build-the-usb4_rdma-provider.patch
Patch1002: 0003-libibverbs-verbs.h-declare-verbs_provider_usb4_rdma.patch
...
%prep
%autosetup -n rdma-core-%{version} -p1
```

Then `rpmbuild -ba rdma-core.spec` produces our-provider-included RPMs.

## Regenerating the patches

When the upstream provider source in `userspace/usb4_rdma/` changes, refresh
the patches:

```sh
./packaging/regen-rdma-core-patches.sh
```

Re-runs `git format-patch` against a fresh rdma-core baseline.
