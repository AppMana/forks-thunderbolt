#!/usr/bin/env bash
# Regenerate the rdma-core patches that ship our libibverbs provider.
#
# Pulls a clean rdma-core source tarball (the version nixpkgs is pinned
# to), checks in our provider sources, and rewrites the numbered patches
# in ./packaging/rdma-core-patches/ that any rdma-core build can apply:
# 0001-0003 from userspace/usb4_rdma/, 0005 from userspace/tbrxe/. 0004
# is hand-maintained (it edits upstream providers/rxe files) and is only
# re-applied here so 0005 is generated on the series packagers use. Run
# this after touching userspace/usb4_rdma/ or userspace/tbrxe/ files.

set -euo pipefail

REPO_ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT="$REPO_ROOT/packaging/rdma-core-patches"

# Pull the exact rdma-core source selected by this flake.
RDMA_SRC=$(nix eval --raw \
    "$REPO_ROOT#packages.x86_64-linux.rdma-core-usb4.src.outPath")
if [ ! -d "$RDMA_SRC" ]; then
    echo "rdma-core source is not a directory: $RDMA_SRC" >&2
    exit 1
fi
echo "Using rdma-core source: $RDMA_SRC"

WORK=$(mktemp -d -t rdma-patches-XXXXXX)
trap 'rm -rf "$WORK"' EXIT

cp -r "$RDMA_SRC"/. "$WORK/"
chmod -R u+w "$WORK"
cd "$WORK"

git init -q
git config user.email "ci@thunderbolt-ibverbs"
git config user.name "thunderbolt-ibverbs"
git config commit.gpgsign false
git add -A
git commit -qm "rdma-core baseline"

# Drop our provider source in.
mkdir -p providers/usb4_rdma
cp -r "$REPO_ROOT/userspace/usb4_rdma/." providers/usb4_rdma/
git add providers/usb4_rdma
git commit -qm "providers/usb4_rdma: add USB4 soft-RDMA provider

Out-of-tree provider for the usb4_rdma kernel module which exposes
Thunderbolt/USB4 host-to-host xdomain links as InfiniBand verbs
devices.

Source: https://github.com/hellas-ai/thunderbolt-ibverbs"

# Wire the provider into the build.
sed -i '/add_subdirectory(providers\/siw)/a add_subdirectory(providers/usb4_rdma)' CMakeLists.txt
git add CMakeLists.txt
git commit -qm "CMakeLists.txt: build the usb4_rdma provider"

# Declare the provider in the public header so the static-link
# all_providers.c indirection sees it. rdma-core hand-maintains this
# list — every in-tree provider has an extern in libibverbs/verbs.h.
sed -i '/extern const struct verbs_device_ops verbs_provider_siw;/a extern const struct verbs_device_ops verbs_provider_usb4_rdma;' libibverbs/verbs.h
git add libibverbs/verbs.h
git commit -qm "libibverbs/verbs.h: declare verbs_provider_usb4_rdma

Required for the static-archive build path (libibverbs/all_providers.c)
to compile when ENABLE_STATIC=1 is on, which is the default on Debian
and most distros."

# Re-apply the hand-maintained rxe patch so the tbrxe commit sits on the
# same series consumers apply. The 0004 file itself is not regenerated.
git am "$OUT"/0004-*.patch

# Drop the tbrxe provider source in and wire it into the build. tbrxe is
# the stock rxe provider compiled under a different identity (see
# userspace/tbrxe/tbrxe.c); the standalone-package path ships it instead
# of the 0004-patched librxe.
mkdir -p providers/tbrxe
cp -r "$REPO_ROOT/userspace/tbrxe/." providers/tbrxe/
git add providers/tbrxe
sed -i '/add_subdirectory(providers\/usb4_rdma)/a add_subdirectory(providers/tbrxe)' CMakeLists.txt
git add CMakeLists.txt
git commit -qF - <<'MSG'
providers/tbrxe: dedicated provider for tbrxe (usb4_rdma*) devices

tbrxe.ko speaks the stock rxe uverbs ABI under a private driver id
(0x55534234, "USB4"). Compile the stock rxe provider sources a second
time under the tbrxe name, with the match table reduced to that driver
id alone and the id stamped into the uverbs ioctl headers.

Unlike patching providers/rxe (0004), the resulting libtbrxe ships as a
standalone package alongside the distro's untouched librxe: the two
providers never contend for a device because each matches a disjoint
driver id.
MSG

mkdir -p "$OUT"
find "$OUT" -name '*.patch' ! -name '0004-*' -delete
git format-patch -o "$OUT" HEAD~5..HEAD~2
git format-patch -o "$OUT" --start-number 5 -1 HEAD

echo ""
echo "Regenerated patches in $OUT:"
ls -la "$OUT"/*.patch
