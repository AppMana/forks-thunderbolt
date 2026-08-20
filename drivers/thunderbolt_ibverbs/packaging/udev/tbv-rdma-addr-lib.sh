# tbv-rdma-addr-lib.sh -- pure per-link address derivation for usb4_rdma rails.
# Sourced by the udev helper and by tests/tbv-rdma-addr-test.sh. POSIX sh.
#
# A cabled link is identified by the unordered pair {local host-router UUID,
# remote XDomain peer UUID}. Sorting the pair makes both ends agree, so they
# derive the SAME routable /64 (a ULA hashed from the pair) and therefore share
# a subnet -- which is exactly the honest-HCA reachability signal the kernel's
# tbv_gid_subnet_match RTR backstop and the rdmaroute NCCL plugin both sense.
# Different links hash to different /64s, so non-neighbours are off-subnet.

# tbv_link_prefix UIDa UIDb -> "fdxx:xxxx:xxxx:xxxx" (order-independent /64)
tbv_link_prefix() {
	_key=$(printf '%s\n%s\n' "$1" "$2" | LC_ALL=C sort | tr -d '\n-')
	_h=$(printf '%s' "$_key" | sha256sum | cut -c1-14)
	printf 'fd%s:%s:%s:%s' \
		"$(printf '%s' "$_h" | cut -c1-2)" \
		"$(printf '%s' "$_h" | cut -c3-6)" \
		"$(printf '%s' "$_h" | cut -c7-10)" \
		"$(printf '%s' "$_h" | cut -c11-14)"
}

# tbv_link_host LOCAL REMOTE -> 1 if LOCAL is the lexicographically-lower UUID,
# else 2. Gives the two ends distinct host parts within the shared /64.
tbv_link_host() {
	_lo=$(printf '%s\n%s\n' "$1" "$2" | LC_ALL=C sort | head -n1)
	[ "$_lo" = "$1" ] && printf '1' || printf '2'
}

# tbv_link_addr LOCAL REMOTE [ROUTE] -> the address this end assigns.
#
# Two hosts: "fd..::H/64" with H from tbv_link_host (ROUTE ignored).
#
# Intra-domain self-loop (docs/tb_same_host.md): LOCAL == REMOTE makes the
# UUID pair degenerate, so the host split comes from ROUTE -- each end's own
# XDomain route (hex, the netdev's tbv_route attr), the one per-link value
# the two cable ends never share. The interface ID is the 64-bit route with
# the top bit set as a marker, keeping the self-loop ID space disjoint from
# the ::1/::2 a normal link uses (route 1 would otherwise render as ::1).
tbv_link_addr() {
	if [ "$1" = "$2" ] && [ -n "${3:-}" ]; then
		_r=$(( 0x$3 ))
		printf '%s:%x:%x:%x:%x/64' "$(tbv_link_prefix "$1" "$2")" \
			$(( ((_r >> 48) & 0xffff) | 0x8000 )) \
			$(( (_r >> 32) & 0xffff )) \
			$(( (_r >> 16) & 0xffff )) \
			$(( _r & 0xffff ))
		return
	fi
	printf '%s::%s/64' "$(tbv_link_prefix "$1" "$2")" "$(tbv_link_host "$1" "$2")"
}
