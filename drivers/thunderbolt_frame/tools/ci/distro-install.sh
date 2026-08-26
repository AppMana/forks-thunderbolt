#!/usr/bin/env bash
# Install a thunderbolt-frame-tools or usb4-rdma-provider package built by
# the matching distro-package*.sh script. Verifies:
#
#   usb4-rdma-provider         — tbrxe provider is installed at the expected path,
#                                its dynamic deps resolve, and libibverbs does
#                                not crash when its driver hint is present.
#   thunderbolt-frame-tools    — the capture harness is installed, parses, and
#                                exposes its documented CLI.
#
# Does NOT load any kernel module.

set -euo pipefail

usage() {
	cat <<'EOF'
Usage:
  tools/ci/distro-install.sh <artefact-path-or-glob>

Detects the package type from the artefact filename
(thunderbolt-frame-tools or usb4-rdma-provider) and runs the appropriate
verification flow.
EOF
}

target="${1:-}"
case "${target:-}" in
	-h|--help) usage; exit 0 ;;
	"") usage >&2; exit 1 ;;
esac

shopt -s nullglob
# shellcheck disable=SC2206
artefacts=( $target )
shopt -u nullglob
if [[ ${#artefacts[@]} -eq 0 ]]; then
	if [[ -f "$target" ]]; then
		artefacts=( "$target" )
	else
		printf 'error: no artefact matched: %s\n' "$target" >&2
		exit 1
	fi
fi
if [[ ${#artefacts[@]} -gt 1 ]]; then
	printf 'error: multiple artefacts matched: %s\n' "${artefacts[*]}" >&2
	exit 1
fi
artefact="$(realpath "${artefacts[0]}")"
[[ -f "$artefact" ]] || { printf 'error: not a file: %s\n' "$artefact" >&2; exit 1; }

case "$(basename "$artefact")" in
	thunderbolt-frame-tools*) pkg_kind="tools" ;;
	usb4-rdma-provider*)       pkg_kind="rdma-provider" ;;
	*)
		printf 'error: unknown package name in %s\n' "$artefact" >&2
		exit 1
		;;
esac

install_provider_deps() {
	if command -v apt-get >/dev/null 2>&1; then
		export DEBIAN_FRONTEND=noninteractive
		apt-get update -qq
		apt-get install -y -qq --no-install-recommends \
			ca-certificates file ibverbs-providers ibverbs-utils libibverbs1
	elif command -v dnf >/dev/null 2>&1; then
		dnf install -y -q --setopt=install_weak_deps=False \
			ca-certificates file libibverbs libibverbs-utils
	elif command -v pacman >/dev/null 2>&1; then
		pacman -Syu --noconfirm --needed \
			ca-certificates file rdma-core
	else
		printf 'error: unsupported distro\n' >&2
		cat /etc/os-release >&2 || true
		exit 1
	fi
}

install_tools_deps() {
	if command -v apt-get >/dev/null 2>&1; then
		export DEBIAN_FRONTEND=noninteractive
		apt-get update -qq
		apt-get install -y -qq --no-install-recommends \
			bash coreutils openssh-client python3
	else
		printf 'error: the tools package is currently packaged for Debian/Ubuntu only\n' >&2
		exit 1
	fi
}

install_package() {
	case "$artefact" in
	*.deb)
		DEBIAN_FRONTEND=noninteractive apt-get install -y -qq "$artefact"
		;;
	*.rpm)
		dnf install -y "$artefact"
		;;
	*.pkg.tar.zst|*.pkg.tar.xz)
		pacman -U --noconfirm "$artefact"
		;;
	*)
		printf 'error: unsupported artefact extension: %s\n' "$artefact" >&2
		exit 1
		;;
	esac
}

verify_provider() {
	install_provider_deps
	install_package

	local driver="/etc/libibverbs.d/tbrxe.driver"
	[[ -f "$driver" ]] ||
		{ printf 'error: driver hint missing at %s\n' "$driver" >&2; exit 1; }

	printf '==> Driver hint: %s\n' "$driver"
	cat "$driver"

	local so=""
	for d in /usr/lib/x86_64-linux-gnu/libibverbs /usr/lib64/libibverbs /usr/lib/libibverbs; do
		[[ -d "$d" ]] || continue
		so="$(find "$d" -maxdepth 1 -name 'libtbrxe-rdmav*.so' -print -quit)"
		[[ -n "$so" ]] && break
	done
	[[ -n "$so" ]] ||
		{ printf 'error: provider .so not found in any libibverbs dir\n' >&2; exit 1; }

	printf '==> Provider .so: %s\n' "$so"
	file "$so"

	printf '==> ldd lib resolution\n'
	if ldd "$so" 2>&1 | grep -E 'not found'; then
		printf 'error: unresolved dynamic library\n' >&2
		exit 1
	fi
	ldd "$so" | sed -n '1,12p'

	# ldd -r performs relocations and reports missing function references.
	# Catches PABI mismatches (e.g. wrong verbs_register_driver_<N> version)
	# and missing imports — without needing an actual /sys/class/infiniband
	# device for libibverbs to match against.
	printf '==> ldd -r symbol resolution\n'
	if ldd -r "$so" 2>&1 | grep -E 'undefined symbol'; then
		printf 'error: unresolved symbols in provider .so\n' >&2
		exit 1
	fi

	printf '==> ibv_devices smoke\n'
	ibv_devices

	printf '==> provider install verification OK\n'
}

verify_tools() {
	install_tools_deps
	install_package
	command -v tbv-hang-repro >/dev/null
	command -v tbv-nhi-ring-regs >/dev/null
	bash -n "$(command -v tbv-hang-repro)"
	tbv-hang-repro --help | grep -q '^Usage:'
	python3 -c 'compile(open("/usr/bin/tbv-nhi-ring-regs", encoding="utf-8").read(), "/usr/bin/tbv-nhi-ring-regs", "exec")'
	printf '==> tools install verification OK\n'
}

case "$pkg_kind" in
	tools)         verify_tools ;;
	rdma-provider) verify_provider ;;
esac
