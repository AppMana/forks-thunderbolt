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

# --- intra-domain self-loop (docs/tb_same_host.md): local uid == remote uid.
# The uid pair is degenerate, so the host split MUST come from the one value
# the two cable ends never share: each end's XDomain route. Contract:
#   * same /64 for both ends (they are one cabled link);
#   * host part derived from the end's own route, so the ends differ;
#   * stable, and never colliding with the ::1/::2 of a normal link.
RA=1      # port 1 route as read from the xdomain (hex, no 0x)
RB=3      # port 3 route
lp="$(tbv_link_prefix "$A" "$A")"
check "selfloop /64 symmetric"    "$(tbv_link_prefix "$A" "$A")" "$lp"
neq   "selfloop ends differ"      "$(tbv_link_addr "$A" "$A" "$RA")" "$(tbv_link_addr "$A" "$A" "$RB")"
check "selfloop end A stable"     "$(tbv_link_addr "$A" "$A" "$RA")" "$(tbv_link_addr "$A" "$A" "$RA")"
# both ends sit in the loop /64
case "$(tbv_link_addr "$A" "$A" "$RA")" in "${lp}:"*) printf 'ok   - selfloop A in loop /64\n' ;;
	*) printf 'FAIL - selfloop A not in %s: %q\n' "$lp" "$(tbv_link_addr "$A" "$A" "$RA")"; fail=1 ;; esac
# never the ::1/::2 host parts a normal two-host link uses
neq   "selfloop A avoids ::1"     "$(tbv_link_addr "$A" "$A" "$RA")" "${lp}::1/64"
neq   "selfloop A avoids ::2"     "$(tbv_link_addr "$A" "$A" "$RA")" "${lp}::2/64"
neq   "selfloop B avoids ::1"     "$(tbv_link_addr "$A" "$A" "$RB")" "${lp}::1/64"
neq   "selfloop B avoids ::2"     "$(tbv_link_addr "$A" "$A" "$RB")" "${lp}::2/64"
# a wide (multi-hop) route still yields a valid distinct address
neq   "selfloop wide routes differ" "$(tbv_link_addr "$A" "$A" "30501")" "$(tbv_link_addr "$A" "$A" "50301")"
# two-host behavior is untouched by the optional route argument
check "two-host ignores route"    "$(tbv_link_addr "$A" "$B" "$RA")" "${p}::1/64"

[ "$fail" = 0 ] && echo "PASS" || echo "FAILED"
exit "$fail"
