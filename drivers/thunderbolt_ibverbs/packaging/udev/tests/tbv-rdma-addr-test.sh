#!/usr/bin/env bash
# Test the pure per-link address derivation used by the usb4_rdma rail udev
# helper. The contract the honest HCA depends on:
#   * symmetry:    both ends of a link (uid pair in either order) derive the
#                  SAME /64 -> they share a subnet (reachable).
#   * host split:  the two ends get DIFFERENT host parts (::1 vs ::2) so the
#                  addresses differ within the shared /64.
#   * uniqueness:  different links (different uid pairs) derive DIFFERENT /64s
#                  -> non-neighbours are off-subnet (ENETUNREACH, no misroute).
#   * stability:   the /64 is a fixed function of the uid pair (no randomness).
set -u
here="$(cd "$(dirname "$0")" && pwd)"
. "${here}/../tbv-rdma-addr-lib.sh"

fail=0
check() { # desc, actual, expected
	if [ "$2" = "$3" ]; then
		printf 'ok   - %s\n' "$1"
	else
		printf 'FAIL - %s: got %q want %q\n' "$1" "$2" "$3"; fail=1
	fi
}
neq() { # desc, a, b  (assert a != b)
	if [ "$2" != "$3" ]; then printf 'ok   - %s\n' "$1"
	else printf 'FAIL - %s: both %q\n' "$1" "$2"; fail=1; fi
}

A=385a8780-0084-b92c-ffff-ffffffffffff   # appmana-018 host router
B=5bdf8780-004d-b184-ffff-ffffffffffff   # appmana-027
C=ca030000-0072-741e-830e-d63714628200   # appmana-002

# symmetry: prefix(A,B) == prefix(B,A)
check "prefix symmetric A,B"      "$(tbv_link_prefix "$A" "$B")" "$(tbv_link_prefix "$B" "$A")"
# host split: the lower uid gets 1, the higher gets 2 -> the two ends differ
check "host of lower uid is 1"    "$(tbv_link_host "$A" "$B")" "1"
check "host of higher uid is 2"   "$(tbv_link_host "$B" "$A")" "2"
neq   "ends get different host"   "$(tbv_link_host "$A" "$B")" "$(tbv_link_host "$B" "$A")"
# uniqueness: different links -> different /64
neq   "distinct links distinct /64" "$(tbv_link_prefix "$A" "$B")" "$(tbv_link_prefix "$A" "$C")"
# stability: same inputs -> same output twice
check "stable across runs"        "$(tbv_link_prefix "$A" "$B")" "$(tbv_link_prefix "$A" "$B")"
# shape: a /64 prefix is fd + 3 more hextets (4 groups), ULA
p="$(tbv_link_prefix "$A" "$B")"
case "$p" in fd??:*:*:*) printf 'ok   - prefix is fd../64-shaped (%s)\n' "$p" ;;
	*) printf 'FAIL - prefix shape: %q\n' "$p"; fail=1 ;; esac
# full address assembles cleanly
check "full addr A->B" "$(tbv_link_addr "$A" "$B")" "${p}::1/64"

[ "$fail" = 0 ] && echo "PASS" || echo "FAILED"
exit "$fail"
