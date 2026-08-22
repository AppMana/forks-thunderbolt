# rdma-core patches

The userspace providers for our kernel engines are out-of-tree libibverbs
providers that plug into rdma-core via the standard `providers/` mechanism.
Rather than fork rdma-core or vendor its internal headers, we ship **minimal
patches** that any rdma-core source tree can apply to produce a build that
includes our providers alongside the upstream ones.

These are build inputs, not patches installed on fleet kernels or on the
runtime `rdma-core` packages. The normal distro packaging script applies
0001-0003 and 0005 to a temporary, version-matched rdma-core source tree,
builds the plugins, and puts only these four files in the standalone
`usb4-rdma-provider` package:

```
/etc/libibverbs.d/usb4_rdma.driver
/etc/libibverbs.d/tbrxe.driver
/usr/lib/<multiarch>/libibverbs/libusb4_rdma-rdmav<PABI>.so
/usr/lib/<multiarch>/libibverbs/libtbrxe-rdmav<PABI>.so
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

0004-providers-rxe-bind-to-usb4_rdma-devices.patch
   Teaches the in-tree `rxe` provider tbrxe's private RDMA driver id.
   Full-distro-rebuild path only; NOT used by the standalone package.

0005-providers-tbrxe-dedicated-provider-for-tbrxe-usb4_rd.patch
   Adds providers/tbrxe/: the stock rxe provider compiled a second time
   under the tbrxe name, matching only tbrxe's private driver id. This is
   what the standalone package ships.
```

## thunderbolt_frame_rxe.ko needs a provider that knows its driver id: 0004 or 0005

Patches 0001-0003 are about the *legacy* `usb4_rdma` engine (kernel
`ibdev.c`). Patches 0004 and 0005 are about `thunderbolt_frame_rxe.ko` (formerly
`tbrxe.ko`), the rxe-derived
engine, which speaks the stock rxe uverbs ABI but not under the stock
driver id.

tbrxe used to register with `RDMA_DRIVER_RXE`, which is how the stock `rxe`
provider found it. It no longer can: `ib_unregister_driver()` unregisters by
driver id with no owner check, so `rmmod rdma_rxe` swept live tbrxe devices
off the node, and tbrxe could not use that fence for its own module exit
without destroying the `rxe_lan` rail. tbrxe now owns
`RDMA_DRIVER_USB4_RDMA` (`rxe/rxe.h`).

The driver id is load-bearing in userspace twice over: rdma-core matches
providers by driver id first (`libibverbs/init.c`, `try_drivers()`), and the
provider stamps its compiled-in id into every uverbs ioctl header, which the
kernel rejects with `EINVAL` on mismatch. A name match alone cannot fix
either. **So the engine module and an id-aware rdma-core provider must be rolled
together: a new tbrxe against a stock rdma-core leaves `usb4_rdma*` devices
with no userspace provider.**

Two patches solve this for two different consumers:

- **0004** adds the id to the in-tree `rxe` provider's match table. That is
  only deliverable by rebuilding the distro's own rdma-core packages (the
  patched `librxe-rdmav<PABI>.so` replaces the stock file, which the
  standalone package cannot do without a dpkg file conflict against
  `ibverbs-providers`). Use it on the full-distro-rebuild path below.
- **0005** builds the same rxe sources a *second* time as a separate
  `tbrxe` provider (`userspace/tbrxe/tbrxe.c` #includes `providers/rxe/rxe.c`
  with the identity rebound) whose match table holds only
  `RDMA_DRIVER_USB4_RDMA`. `libtbrxe` installs beside the distro's untouched
  `librxe`; they never contend for a device because driver-id matching is
  disjoint (stock rxe matches `RDMA_DRIVER_RXE` and the `rxe` name; tbrxe
  matches only the USB4 id and has no name match). This is what
  `tools/ci/distro-package-rdma.sh` ships — it applies 0001-0003 and 0005,
  skipping 0004.

Applying both (as a full distro rebuild may) is harmless: a tbrxe device
then has two willing providers with identical behavior, and libibverbs picks
whichever registered first.

The patches are auto-generated from the source in `userspace/usb4_rdma/`
(0001-0003) and `userspace/tbrxe/` (0005) via `git format-patch`; 0004 is
hand-maintained. All apply cleanly against upstream rdma-core ≥ v62.

## How each consumer applies them

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

When the provider source in `userspace/usb4_rdma/` or `userspace/tbrxe/`
changes, refresh the patches:

```sh
./packaging/regen-rdma-core-patches.sh
```

Re-runs `git format-patch` against a fresh rdma-core baseline.
