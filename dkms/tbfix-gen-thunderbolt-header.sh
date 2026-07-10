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

python3 - "$src" "$out/linux/thunderbolt.h" <<'PY'
import pathlib
import sys

src, dst = sys.argv[1], sys.argv[2]
text = pathlib.Path(src).read_text()

# 1) struct completion is needed for tb_nhi::domain_released below.
if "#include <linux/completion.h>" not in text:
    needle = "#include <linux/workqueue.h>\n"
    if needle not in text:
        raise SystemExit("tbfix header shim: workqueue include anchor not found")
    text = text.replace(needle, needle + "#include <linux/completion.h>\n", 1)

# 2) Source-aware XDomain protocol handler (callback_xd) + capability macro.
if "TB_PROTOCOL_HANDLER_HAS_XDOMAIN" not in text:
    old = ("\tint (*callback)(const void *buf, size_t size, void *data);\n"
           "\tvoid *data;")
    new = ("\tint (*callback)(const void *buf, size_t size, void *data);\n"
           "#define TB_PROTOCOL_HANDLER_HAS_XDOMAIN 1\n"
           "\tint (*callback_xd)(struct tb_xdomain *xd, const void *buf,"
           " size_t size,\n"
           "\t\t\t   void *data);\n"
           "\tvoid *data;")
    if old not in text:
        raise SystemExit("tbfix header shim: protocol handler anchor not found")
    text = text.replace(old, new, 1)

# 3) tb_nhi::domain_released completion (nhi.c/domain.c reference it directly).
if "domain_released" not in text:
    old = "\tunsigned long quirks;\n};"
    new = ("\tunsigned long quirks;\n"
           "\tstruct completion domain_released;\n};")
    if old not in text:
        raise SystemExit("tbfix header shim: tb_nhi anchor not found")
    text = text.replace(old, new, 1)

pathlib.Path(dst).write_text(text)
PY
