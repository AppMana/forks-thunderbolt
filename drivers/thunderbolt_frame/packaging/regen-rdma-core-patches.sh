#!/usr/bin/env bash
# Regenerate the rdma-core patches that ship our libibverbs provider.
#
# Pulls a clean rdma-core source tree (RDMA_CORE_TAG, default v62.0 — the
# tag distro-package-rdma.sh builds), checks in our provider sources, and
# rewrites 0005 in ./packaging/rdma-core-patches/ from userspace/tbrxe/.
# 0004 is hand-maintained (it edits upstream providers/rxe files) and is
# re-applied here so 0005 is generated on the series packagers use. Run this
# after touching userspace/tbrxe/ files.

set -euo pipefail

REPO_ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT="$REPO_ROOT/packaging/rdma-core-patches"

# Pull a clean rdma-core source at the same tag the provider package builds
# against (tools/ci/distro-package-rdma.sh).
RDMA_CORE_TAG="${RDMA_CORE_TAG:-v62.0}"

WORK=$(mktemp -d -t rdma-patches-XXXXXX)
trap 'rm -rf "$WORK"' EXIT

echo "Using rdma-core $RDMA_CORE_TAG"
git clone -q --depth 1 --branch "$RDMA_CORE_TAG" \
    https://github.com/linux-rdma/rdma-core "$WORK"
cd "$WORK"
rm -rf .git

git init -q
git config user.email "ci@forks-thunderbolt"
git config user.name "forks-thunderbolt"
git config commit.gpgsign false
git add -A
git commit -qm "rdma-core baseline"

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
sed -i '/add_subdirectory(providers\/siw)/a add_subdirectory(providers/tbrxe)' CMakeLists.txt
git add CMakeLists.txt
git commit -qF - <<'MSG'
providers/tbrxe: dedicated provider for tbrxe (usb4_rdma*) devices

thunderbolt_frame_rxe.ko (formerly tbrxe.ko) speaks the stock rxe uverbs
ABI under a private driver id
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
git format-patch -o "$OUT" --start-number 5 -1 HEAD

echo ""
echo "Regenerated patches in $OUT:"
ls -la "$OUT"/*.patch
