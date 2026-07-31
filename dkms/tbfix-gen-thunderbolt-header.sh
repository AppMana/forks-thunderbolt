#!/bin/sh
# Generate the fork-patched <linux/thunderbolt.h> into a build-local include dir
# that shadows the kernel's copy (the caller prepends -I<out> to LINUXINCLUDE).
#
# The fork vendors the ENTIRE drivers/thunderbolt/ subsystem and rebuilds it
# out-of-tree as a complete replacement, so it must own the subsystem's PUBLIC
# header too. The vendored .c files reference members the stock header lacks:
#   - struct tb_nhi::domain_released   (nhi.c, domain.c: unconditional)
#   - struct tb_protocol_handler::callback_xd + TB_PROTOCOL_HANDLER_HAS_XDOMAIN
#     (xdomain.c: unconditional; thunderbolt_ibverbs: source-aware handler)
#
# Rather than mutate the node's kernel headers out-of-band (the ansible
# lineinfile/blockinfile anti-pattern this replaces), read the kernel's OWN
# header and layer ONLY these additive changes into a build-local copy. This
# tracks the stock header per kernel automatically and is fully self-contained:
# nothing outside the DKMS build tree is written. Idempotent (a fresh copy is
# taken each run) and the SINGLE source of the header shim -- run-kunit.sh calls
# it too, so the KUnit overlay and the DKMS build never drift.
#
# POSIX sh + awk ONLY. A DKMS kernel-module build runs in a minimal environment
# (CI's ubuntu:24.04 container, a new-kernel autoinstall on a node): it has
# make/coreutils/awk/sed but NOT python3. Do not reintroduce a python dependency.
#
# Usage: tbfix-gen-thunderbolt-header.sh <kernel_source_dir> <out_include_dir>
set -eu

ksrc="${1:-}"
out="${2:-}"
if [ -z "$ksrc" ] || [ -z "$out" ]; then
	echo "usage: $0 <kernel_source_dir> <out_include_dir>" >&2
	exit 2
fi

src="$ksrc/include/linux/thunderbolt.h"
if [ ! -f "$src" ]; then
	echo "tbfix header shim: kernel header not found: $src" >&2
	exit 1
fi

mkdir -p "$out/linux"
dst="$out/linux/thunderbolt.h"

# Idempotency: only apply a change the stock header does not already carry.
add_completion=1; grep -q '#include <linux/completion.h>' "$src" && add_completion=0
add_handler=1;    grep -q 'TB_PROTOCOL_HANDLER_HAS_XDOMAIN' "$src" && add_handler=0
add_nhi=1;        grep -q 'domain_released' "$src" && add_nhi=0
add_paths_active=1; grep -q 'TB_XDOMAIN_HAS_PATHS_ACTIVE' "$src" && add_paths_active=0
add_reannounce=1; grep -q 'TB_XDOMAIN_HAS_REANNOUNCE' "$src" && add_reannounce=0

# Struct-scoped transformation. Anchors:
#   1. after "#include <linux/workqueue.h>"           -> add completion.h
#   2. inside struct tb_protocol_handler, after the
#      "int (*callback)(...)" line, BEFORE ->data     -> insert macro+callback_xd
#   3. inside struct tb_nhi, before its closing "};"   -> append domain_released
#   4. after the tb_xdomain_disable_paths() declaration (first line matched,
#      insertion after its closing ");")               -> add paths_active decl
awk -v add_completion="$add_completion" \
    -v add_handler="$add_handler" \
    -v add_nhi="$add_nhi" \
    -v add_paths_active="$add_paths_active" \
    -v add_reannounce="$add_reannounce" '
	add_reannounce == 1 && \
	    $0 == "void tb_unregister_property_dir(const char *key, struct tb_property_dir *dir);" {
		print
		print "#define TB_XDOMAIN_HAS_REANNOUNCE 1"
		print "void tb_reannounce_property_dirs(void);"
		next
	}
	add_completion == 1 && $0 == "#include <linux/workqueue.h>" {
		print
		print "#include <linux/completion.h>"
		next
	}
	/^struct tb_protocol_handler \{/ { in_ph = 1 }
	/^struct tb_nhi \{/            { in_nhi = 1 }
	add_handler == 1 && in_ph == 1 && \
	    $0 ~ /int \(\*callback\)\(const void \*buf, size_t size, void \*data\);/ {
		print
		print "#define TB_PROTOCOL_HANDLER_HAS_XDOMAIN 1"
		print "\tint (*callback_xd)(struct tb_xdomain *xd, const void *buf, size_t size,"
		print "\t\t\t   void *data);"
		next
	}
	add_nhi == 1 && in_nhi == 1 && $0 == "};" {
		print "\tstruct completion domain_released;"
		print
		in_nhi = 0
		next
	}
	in_ph == 1 && $0 == "};" { in_ph = 0 }
	add_paths_active == 1 && \
	    $0 ~ /^int tb_xdomain_disable_paths\(struct tb_xdomain \*xd, int transmit_path,/ {
		in_disable = 1
	}
	add_paths_active == 1 && in_disable == 1 && $0 ~ /\);$/ {
		print
		print "#define TB_XDOMAIN_HAS_PATHS_ACTIVE 1"
		print "int tb_xdomain_paths_active(struct tb_xdomain *xd, int transmit_path,"
		print "\t\t\t    int transmit_ring, int receive_path,"
		print "\t\t\t    int receive_ring);"
		in_disable = 0
		next
	}
	{ print }
' "$src" > "$dst"

# Post-condition: the generated header MUST carry all three additions, whether
# they were already present or just inserted. A miss means an anchor did not
# match (upstream header reshaped) -- fail loudly rather than emit a header that
# compiles the vendored subsystem to a broken layout.
fail=0
for tok in '#include <linux/completion.h>' 'TB_PROTOCOL_HANDLER_HAS_XDOMAIN' 'domain_released' 'TB_XDOMAIN_HAS_PATHS_ACTIVE' 'TB_XDOMAIN_HAS_REANNOUNCE'; do
	if ! grep -q "$tok" "$dst"; then
		echo "tbfix header shim: anchor for '$tok' not found in $src" >&2
		fail=1
	fi
done
[ "$fail" -eq 0 ] || { rm -f "$dst"; exit 1; }
