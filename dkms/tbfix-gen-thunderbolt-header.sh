#!/bin/sh
# Generate the fork-patched <linux/thunderbolt.h> into a build-local include dir
# that shadows the kernel's copy (the caller prepends -I<out> to LINUXINCLUDE).
#
# The fork vendors the ENTIRE drivers/thunderbolt/ subsystem and rebuilds it
# out-of-tree as a complete replacement, so it must own the subsystem's PUBLIC
# header too. The vendored .c files reference members the stock header lacks:
#   - struct tb_protocol_handler::callback_xd + TB_PROTOCOL_HANDLER_HAS_XDOMAIN
#     (xdomain.c: unconditional; thunderbolt_frame: source-aware handler)
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
add_handler=1;    grep -q 'TB_PROTOCOL_HANDLER_HAS_XDOMAIN' "$src" && add_handler=0
add_paths_active=1; grep -q 'TB_XDOMAIN_HAS_PATHS_ACTIVE' "$src" && add_paths_active=0
add_path_quarantine=1; grep -q 'TB_XDOMAIN_HAS_PATH_QUARANTINE' "$src" && add_path_quarantine=0
add_reannounce=1; grep -q 'TB_XDOMAIN_HAS_REANNOUNCE' "$src" && add_reannounce=0
# struct tb_xdomain::removing (xdomain.c: mainline 2c5d2d3c3f70 backport --
# tb_xdomain_remove() sets it under xd->lock; every external
# queue_delayed_work site checks it to close the cancel/requeue UAF race)
add_removing=1;   grep -q 'bool removing;' "$src" && add_removing=0
# struct tb_ring::wait + tb_ring_flush() (nhi.c: mainline 94a11cd5ddb1
# backport -- lets service drivers wait for in-flight frames to complete
# before stopping a ring)
add_ring_flush=1; grep -q 'tb_ring_flush' "$src" && add_ring_flush=0
# A terminal service-driver teardown may have to hand a stopped ring to the
# core when the fabric path cannot be proven drained. The ring stays allocated
# and non-reusable until the domain has obtained real controller-reset proof.
add_ring_quarantine=1; grep -q 'tb_ring_quarantine' "$src" && add_ring_quarantine=0
# struct tb_xdomain::bonding_rearm_attempts (xdomain.c: bounded lane-bonding
# re-arm from ENUMERATED; appended at the END of the struct so every stock
# member keeps its stock offset for stock-header consumers)
add_rearm=1;      grep -q 'bonding_rearm_attempts' "$src" && add_rearm=0
# A firmware topology-event UUID is only a candidate until the peer answers a
# route-local UUID request.
add_uuid_verified=1; grep -q 'bool uuid_verified;' "$src" && add_uuid_verified=0
# Separate failure history for unverified-peer discovery. This must not share
# the properties-announcement counter because both delayed works run at once.
add_uuid_retry=1; grep -q 'uuid_retry_failures' "$src" && add_uuid_retry=0
# Preserve a failed core path teardown across the later is_unplugged service
# unbind phase; consumers must not turn an unresolved tuple into success.
add_path_teardown_err=1; grep -q 'path_teardown_err' "$src" && add_path_teardown_err=0

# Struct-scoped transformation. Anchors:
#   1. inside struct tb_protocol_handler, after the
#      "int (*callback)(...)" line, BEFORE ->data     -> insert macro+callback_xd
#   2. after the tb_xdomain_disable_paths() declaration (first line matched,
#      insertion after its closing ");")               -> add paths_active decl
awk -v add_handler="$add_handler" \
    -v add_paths_active="$add_paths_active" \
    -v add_path_quarantine="$add_path_quarantine" \
    -v add_reannounce="$add_reannounce" \
    -v add_removing="$add_removing" \
    -v add_ring_flush="$add_ring_flush" \
    -v add_ring_quarantine="$add_ring_quarantine" \
    -v add_rearm="$add_rearm" \
    -v add_uuid_verified="$add_uuid_verified" \
    -v add_uuid_retry="$add_uuid_retry" \
    -v add_path_teardown_err="$add_path_teardown_err" '


	add_reannounce == 1 && \
	    $0 == "void tb_unregister_property_dir(const char *key, struct tb_property_dir *dir);" {
		print
		print "#define TB_XDOMAIN_HAS_REANNOUNCE 1"
		print "void tb_reannounce_property_dirs(void);"
		next
	}
	/^struct tb_protocol_handler \{/ { in_ph = 1 }
	/^struct tb_xdomain \{/        { in_xd = 1 }
	add_removing == 1 && in_xd == 1 && $0 == "\tbool is_unplugged;" {
		print
		print "\tbool removing;"
		next
	}
	in_xd == 1 && $0 == "};" {
		if (add_rearm == 1)
			print "\tunsigned int bonding_rearm_attempts;"
		if (add_uuid_verified == 1)
			print "\tbool uuid_verified;"
		if (add_uuid_retry == 1)
			print "\tunsigned int uuid_retry_failures;"
		if (add_path_teardown_err == 1)
			print "\tint path_teardown_err;"
		print "};"
		in_xd = 0
		next
	}
	/^struct tb_ring \{/           { in_ring = 1 }
	in_ring == 1 && $0 == "};" {
		if (add_ring_flush == 1)
			print "\twait_queue_head_t wait;"
		if (add_ring_quarantine == 1)
			print "\tbool quarantined;"
		print "};"
		in_ring = 0
		next
	}
	add_ring_flush == 1 && $0 == "void tb_ring_start(struct tb_ring *ring);" {
		print
		print "#define TB_RING_HAS_FLUSH 1"
		print "bool tb_ring_flush(struct tb_ring *ring, unsigned int timeout_msec);"
		if (add_ring_quarantine == 1) {
			print "#define TB_RING_HAS_QUARANTINE 1"
			print "void tb_ring_quarantine(struct tb_ring *ring);"
		}
		next
	}
	add_ring_flush == 0 && add_ring_quarantine == 1 && \
	    $0 == "void tb_ring_start(struct tb_ring *ring);" {
		print
		print "#define TB_RING_HAS_QUARANTINE 1"
		print "void tb_ring_quarantine(struct tb_ring *ring);"
		next
	}
	# ->callback_xd is APPENDED after ->list, never inserted mid-struct: a
	# consumer module compiled against the STOCK header but linked with the
	# tbfix Module.symvers loads cleanly (CRCs come from the symvers file)
	# yet lays out uuid/callback/data/list at the stock offsets and its
	# storage ends at ->list. A mid-struct insertion shifted ->data onto the
	# offset the core reads as ->callback_xd, and the dispatch walk can jump
	# through the registrant data pointer. With the member appended, every stock field keeps
	# its stock offset and the core only reads ->callback_xd for registrants
	# that left ->callback NULL (which therefore provide the extended
	# struct). NOTE: this awk program lives in a single-quoted sh string;
	# no apostrophes in comments.
	add_handler == 1 && in_ph == 1 && $0 == "};" {
		print "#define TB_PROTOCOL_HANDLER_HAS_XDOMAIN 1"
		print "\tint (*callback_xd)(struct tb_xdomain *xd, const void *buf, size_t size,"
		print "\t\t\t   void *data);"
		print "};"
		in_ph = 0
		next
	}
	in_ph == 1 && $0 == "};" { in_ph = 0 }
	(add_paths_active == 1 || add_path_quarantine == 1) && \
	    $0 ~ /^int tb_xdomain_disable_paths\(struct tb_xdomain \*xd, int transmit_path,/ {
		in_disable = 1
	}
	in_disable == 1 && $0 ~ /\);$/ {
		print
		if (add_paths_active == 1) {
			print "#define TB_XDOMAIN_HAS_PATHS_ACTIVE 1"
			print "int tb_xdomain_paths_active(struct tb_xdomain *xd,"
			print "\t\t\t    int transmit_path, int transmit_ring,"
			print "\t\t\t    int receive_path,"
			print "\t\t\t    int receive_ring);"
		}
		if (add_path_quarantine == 1) {
			print "#define TB_XDOMAIN_HAS_PATH_QUARANTINE 1"
			print "struct tb_ring;"
			print "int tb_xdomain_quarantine_paths(struct tb_xdomain *xd,"
			print "\t\t\t\tint transmit_path, int transmit_ring,"
			print "\t\t\t\tstruct tb_ring *transmit_dma_ring,"
			print "\t\t\t\tint receive_path, int receive_ring,"
			print "\t\t\t\tstruct tb_ring *receive_dma_ring);"
		}
		in_disable = 0
		next
	}
	{ print }
' "$src" > "$dst"

# Post-condition: the generated header MUST carry every addition, whether
# they were already present or just inserted. A miss means an anchor did not
# match (upstream header reshaped) -- fail loudly rather than emit a header that
# compiles the vendored subsystem to a broken layout.
fail=0
for tok in \
	'TB_PROTOCOL_HANDLER_HAS_XDOMAIN' \
	'TB_XDOMAIN_HAS_PATHS_ACTIVE' \
	'TB_XDOMAIN_HAS_PATH_QUARANTINE' \
	'TB_XDOMAIN_HAS_REANNOUNCE' \
	'bool removing;' \
	'TB_RING_HAS_FLUSH' \
	'TB_RING_HAS_QUARANTINE' \
	'bool quarantined;' \
	'bonding_rearm_attempts' \
	'bool uuid_verified;' \
	'uuid_retry_failures' \
	'path_teardown_err'
do
	if ! grep -q "$tok" "$dst"; then
		echo "tbfix header shim: anchor for '$tok' not found in $src" >&2
		fail=1
	fi
done
[ "$fail" -eq 0 ] || { rm -f "$dst"; exit 1; }
