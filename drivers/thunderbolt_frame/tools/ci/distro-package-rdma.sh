#!/usr/bin/env bash
# Build the usb4_rdma libibverbs userspace provider and package it as a native
# .deb / .rpm / .pkg.tar.zst. The provider .so is built against the same
# rdma-core source as the target distro's libibverbs so the PABI version
# matches and apt/dnf/pacman can install the package as a drop-in.
#
#   debian      → upstream rdma-core v62.0 (Debian sid ships current)
#   ubuntu      → distro's own rdma-core via apt-get source (handles 22.04+
#                 where stock libibverbs PABI is older than v62.0)
#   fedora|arch → upstream rdma-core v62.0

set -euo pipefail

usage() {
	cat <<'EOF'
Usage:
  tools/ci/distro-package-rdma.sh debian|ubuntu|fedora|arch

Outputs the produced .deb / .rpm / .pkg.tar.zst into OUT_DIR. For ubuntu the
filename includes the distro codename (e.g. usb4-rdma-provider_0.2.0~jammy_amd64.deb).

Environment:
  TBV_VERSION       Override base version (default reads packaging/version).
  OUT_DIR           Output directory (default $PWD/dist).
  WORK_DIR          Scratch directory (default mktemp).
  RDMA_CORE_TAG     rdma-core git tag for the upstream-source distros (default v62.0).
                    Ignored for ubuntu (uses apt-get source).
  TBV_SKIP_DEPS     Skip distro deps install (default 0).
  TBV_SKIP_BUILD    Skip the rdma-core build step (used by the arch builder
                    re-exec; default 0).
EOF
}

distro="${1:-}"
case "${distro:-}" in
	-h|--help) usage; exit 0 ;;
	debian|ubuntu|fedora|arch) ;;
	"") usage >&2; exit 1 ;;
	*) printf 'error: unsupported distro: %s\n' "$distro" >&2; exit 1 ;;
esac

repo_root="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
base_version="${TBV_VERSION:-$(tr -d '[:space:]' < "$repo_root/packaging/version")}"
[[ -n "$base_version" ]] || { printf 'error: could not determine version\n' >&2; exit 1; }

# Ubuntu builds encode the codename in the version so different ubuntu releases'
# .debs don't collide on the GitHub Releases page and apt picks the right one.
codename=""
if [[ "$distro" == "ubuntu" && -r /etc/os-release ]]; then
	codename="$(. /etc/os-release && printf '%s' "${VERSION_CODENAME:-${UBUNTU_CODENAME:-unknown}}")"
fi
if [[ -n "$codename" ]]; then
	version="${base_version}~${codename}"
else
	version="$base_version"
fi

out_dir="${OUT_DIR:-$repo_root/dist}"
work_dir="${WORK_DIR:-$(mktemp -d)}"
rdma_core_tag="${RDMA_CORE_TAG:-v62.0}"
skip_deps="${TBV_SKIP_DEPS:-0}"
skip_build="${TBV_SKIP_BUILD:-0}"
pkgname="usb4-rdma-provider"

mkdir -p "$out_dir" "$work_dir"

install_deps() {
	[[ "$skip_deps" == "1" ]] && return 0
	case "$distro" in
		debian)
			export DEBIAN_FRONTEND=noninteractive
			apt-get update -qq
			apt-get install -y -qq --no-install-recommends \
				build-essential ca-certificates cmake dpkg-dev \
				git libcap-dev libnl-3-dev libnl-route-3-dev libsystemd-dev \
				libudev-dev libssl-dev ninja-build patch patchelf pkg-config \
				python3-docutils python3-pyelftools
			;;
		ubuntu)
			export DEBIAN_FRONTEND=noninteractive
			# Enable deb-src so apt-get source can fetch rdma-core. Ubuntu 22.04
			# uses /etc/apt/sources.list; 24.04 uses /etc/apt/sources.list.d/ubuntu.sources.
			if [[ -f /etc/apt/sources.list.d/ubuntu.sources ]]; then
				sed -i 's/^Types: deb$/Types: deb deb-src/' /etc/apt/sources.list.d/ubuntu.sources
			else
				sed -i 's/^# deb-src/deb-src/' /etc/apt/sources.list
			fi
			apt-get update -qq
			apt-get install -y -qq --no-install-recommends \
				build-essential ca-certificates cmake dpkg-dev \
				libcap-dev libnl-3-dev libnl-route-3-dev libsystemd-dev \
				libudev-dev libssl-dev ninja-build patch patchelf pkg-config \
				python3-docutils python3-pyelftools
			;;
		fedora)
			dnf install -y -q --setopt=install_weak_deps=False \
				cmake gcc gcc-c++ git libcap-devel libnl3-devel libudev-devel \
				make ninja-build openssl-devel patch patchelf pkgconf rpm-build \
				python3-docutils python3-pyelftools systemd-devel tar
			;;
		arch)
			pacman -Syu --noconfirm --needed \
				base-devel ca-certificates cmake git libnl ninja patch \
				patchelf python-docutils python-pyelftools sudo systemd
			;;
	esac
}

# Populate $src with the rdma-core source tree, patched. Strategy depends on
# distro: ubuntu uses its own packaged source so PABI matches stock libibverbs;
# everything else uses upstream v62.0.
fetch_rdma_core_source() {
	local src="$1"
	rm -rf "$src"
	if [[ "$distro" == "ubuntu" ]]; then
		mkdir -p "$src"
		( cd "$src" && apt-get source rdma-core >/dev/null )
		# apt-get source extracts to ./rdma-core-<ver>/; move it up so $src
		# itself is the source root.
		local extracted
		extracted="$(find "$src" -maxdepth 1 -type d -name 'rdma-core-*' -print -quit)"
		[[ -n "$extracted" ]] ||
			{ printf 'error: apt-get source did not extract rdma-core\n' >&2; exit 1; }
		mv "$extracted"/* "$extracted"/.[!.]* "$src"/ 2>/dev/null || true
		rmdir "$extracted"
	else
		git clone --depth 1 --branch "$rdma_core_tag" \
			https://github.com/linux-rdma/rdma-core "$src"
	fi
}

build_provider() {
	[[ "$skip_build" == "1" ]] && return 0
	local src="$work_dir/rdma-core"
	local build="$src/build"

	fetch_rdma_core_source "$src"

	# Our patches were generated against v62.0 but apply (with offset/fuzz) to
	# older rdma-core down to at least v39 (Ubuntu 22.04). Patch 1 only adds
	# new files; 2 and 3 absorb hunk drift automatically.
	#
	# 0004 patches the in-tree rxe provider in place and serves the
	# full-distro-rebuild path only. The standalone package ships the
	# dedicated tbrxe provider from 0005 instead, leaving the build's librxe
	# stock (it is not packaged either way).
	for p in "$repo_root/packaging/rdma-core-patches"/*.patch; do
		[[ "$(basename "$p")" == 0004-* ]] && continue
		( cd "$src" && patch --silent -p1 < "$p" )
	done

	# The patch series is a distributable snapshot of the canonical provider
	# sources. Refuse to package if it has drifted; otherwise a syntactically
	# valid stale patch can quietly build a useless provider.
	for provider in usb4_rdma tbrxe; do
		if ! diff -ru "$repo_root/userspace/$provider" \
				"$src/providers/$provider"; then
			printf 'error: rdma-core provider patch is stale; run packaging/regen-rdma-core-patches.sh\n' >&2
			exit 1
		fi
	done

	rm -rf "$build"
	mkdir "$build"
	# NO_MAN_PAGES because Ubuntu's apt-get source tree ships a pandoc cache
	# that doesn't match all manpage inputs after our patches; we don't need
	# manpages in the artefact anyway.
	( cd "$build" && cmake -GNinja -DNO_PYVERBS=1 -DNO_MAN_PAGES=1 .. >/dev/null && ninja )

	local so
	for provider in usb4_rdma tbrxe; do
		so="$(find "$build/lib" -maxdepth 1 -name "lib${provider}-rdmav*.so" -print -quit)"
		[[ -n "$so" ]] || { printf 'error: %s provider .so not produced\n' "$provider" >&2; exit 1; }
		if ! nm "$so" | awk -v sym="verbs_provider_${provider}" \
				'$3 == sym { found = 1 } END { exit !found }'; then
			printf 'error: provider registration symbol missing from %s\n' "$so" >&2
			exit 1
		fi

		# CMake embeds the build dir as RUNPATH so libibverbs can run from
		# the build tree without installing. For packaging we want a clean
		# .so with no build-host paths leaked — strip the RUNPATH.
		patchelf --remove-rpath "$so"

		printf '==> Built provider: %s\n' "$(basename "$so")"
	done

	# Both .sos must carry the same PABI suffix or libibverbs would refuse
	# to load one of them.
	local pabis
	pabis="$(find "$build/lib" -maxdepth 1 \
			\( -name 'libusb4_rdma-rdmav*.so' -o -name 'libtbrxe-rdmav*.so' \) |
		sed 's/.*-rdmav\([0-9]*\)\.so/\1/' | sort -u)"
	[[ "$(wc -l <<<"$pabis")" -eq 1 ]] ||
		{ printf 'error: provider PABI mismatch: %s\n' "$pabis" >&2; exit 1; }
}

stage_provider_files() {
	local stage="$1"
	local src="$work_dir/rdma-core"
	local build="$src/build"

	install -d -m 0755 "$stage"

	local provider so
	for provider in usb4_rdma tbrxe; do
		so="$(find "$build/lib" -maxdepth 1 -name "lib${provider}-rdmav*.so" -print -quit)"
		[[ -n "$so" ]] || { printf 'error: %s provider .so not in build tree\n' "$provider" >&2; exit 1; }

		cp "$so" "$stage/"
		# The .driver file emitted by rdma-core's build tree embeds the
		# absolute build-tree path so libibverbs can run from the build dir
		# without install. For packaging, write the plain installed-tree
		# form: `driver <name>`.
		printf 'driver %s\n' "$provider" > "$stage/${provider}.driver"
	done
}

substitute() {
	sed "s/@VERSION@/${version}/g" "$1" > "$2"
}

build_deb() {
	local arch
	arch="$(dpkg-architecture -q DEB_HOST_MULTIARCH)"
	local deb_stage="$work_dir/deb"
	rm -rf "$deb_stage"
	install -d -m 0755 "$deb_stage/DEBIAN"
	install -d -m 0755 "$deb_stage/usr/lib/$arch/libibverbs"
	install -d -m 0755 "$deb_stage/etc/libibverbs.d"

	local files="$work_dir/files"
	stage_provider_files "$files"

	install -m 0644 "$files"/libusb4_rdma-rdmav*.so "$files"/libtbrxe-rdmav*.so \
		"$deb_stage/usr/lib/$arch/libibverbs/"
	install -m 0644 "$files/usb4_rdma.driver" \
		"$deb_stage/etc/libibverbs.d/usb4_rdma.driver"
	install -m 0644 "$files/tbrxe.driver" \
		"$deb_stage/etc/libibverbs.d/tbrxe.driver"

	# udev: stable per-link name + deterministic per-link /64 (ULA) for the
	# u4r* rail netdevs the engine publishes. Shipped here (not in the dkms
	# package) because every RDMA node installs the provider; the fleet
	# tbv-rail-gids oneshot also execs tbv-rdma-addr by this path.
	install -D -m 0644 "$repo_root/packaging/udev/60-usb4-rdma-net.rules" \
		"$deb_stage/usr/lib/udev/rules.d/60-usb4-rdma-net.rules"
	install -D -m 0755 "$repo_root/packaging/udev/tbv-rdma-ifname" \
		"$deb_stage/usr/lib/usb4-rdma/tbv-rdma-ifname"
	install -D -m 0755 "$repo_root/packaging/udev/tbv-rdma-addr" \
		"$deb_stage/usr/lib/usb4-rdma/tbv-rdma-addr"
	install -D -m 0644 "$repo_root/packaging/udev/tbv-rdma-addr-lib.sh" \
		"$deb_stage/usr/lib/usb4-rdma/tbv-rdma-addr-lib.sh"

	substitute "$repo_root/packaging/debian/control-rdma" "$deb_stage/DEBIAN/control"
	substitute "$repo_root/packaging/debian/postinst-rdma" "$deb_stage/DEBIAN/postinst"
	chmod 0755 "$deb_stage/DEBIAN/postinst"

	local deb="$out_dir/${pkgname}_${version}_amd64.deb"
	dpkg-deb --root-owner-group --build "$deb_stage" "$deb" >/dev/null
	printf '==> Built %s\n' "$deb"
}

build_rpm() {
	local rpm_top="$work_dir/rpmbuild"
	rm -rf "$rpm_top"
	install -d -m 0755 "$rpm_top"/{BUILD,RPMS,SOURCES,SPECS,SRPMS}

	stage_provider_files "$rpm_top/SOURCES"

	substitute "$repo_root/packaging/rpm/${pkgname}.spec" \
		"$rpm_top/SPECS/${pkgname}.spec"

	rpmbuild --define "_topdir $rpm_top" -bb \
		"$rpm_top/SPECS/${pkgname}.spec"

	local rpm
	rpm="$(find "$rpm_top/RPMS" -name '*.rpm' -print -quit)"
	[[ -n "$rpm" ]] || { printf 'error: rpmbuild produced no .rpm\n' >&2; exit 1; }

	cp "$rpm" "$out_dir/"
	printf '==> Built %s\n' "$out_dir/$(basename "$rpm")"
}

build_arch_as_builder() {
	local stage="$work_dir/arch"
	rm -rf "$stage"
	install -d -m 0755 "$stage"

	stage_provider_files "$stage"

	local soname
	soname="$(basename "$(find "$stage" -name 'libusb4_rdma-rdmav*.so' -print -quit)")"
	[[ -n "$soname" ]] || { printf 'error: staged .so missing\n' >&2; exit 1; }

	sed -e "s/@VERSION@/${version}/g" -e "s/@SONAME@/${soname}/g" \
		"$repo_root/packaging/arch/PKGBUILD-rdma" > "$stage/PKGBUILD"

	( cd "$stage" && makepkg --noconfirm --skipchecksums --nodeps )

	local pkg
	pkg="$(find "$stage" -maxdepth 1 -name "${pkgname}-[0-9]*.pkg.tar.zst" -print -quit)"
	[[ -n "$pkg" ]] || { printf 'error: makepkg produced no main package\n' >&2; exit 1; }

	cp "$pkg" "$out_dir/"
	printf '==> Built %s\n' "$out_dir/$(basename "$pkg")"
}

build_arch() {
	# makepkg refuses to run as root. Build rdma-core as root (above), then
	# re-exec as a builder user for the packaging step.
	if [[ "$(id -u)" -eq 0 ]]; then
		id -u builder >/dev/null 2>&1 || useradd -m -s /bin/bash builder
		chown -R builder:builder "$work_dir" "$out_dir"
		exec sudo -u builder \
			env TBV_VERSION="$version" OUT_DIR="$out_dir" \
				WORK_DIR="$work_dir" RDMA_CORE_TAG="$rdma_core_tag" \
				TBV_SKIP_DEPS=1 TBV_SKIP_BUILD=1 \
			bash "$0" "$distro"
	fi
	build_arch_as_builder
}

install_deps
build_provider

case "$distro" in
	debian|ubuntu) build_deb ;;
	fedora) build_rpm ;;
	arch)   build_arch ;;
esac
