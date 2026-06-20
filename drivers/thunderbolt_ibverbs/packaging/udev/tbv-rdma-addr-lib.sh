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

# tbv_link_addr LOCAL REMOTE -> "fd..::H/64", the address this end assigns.
tbv_link_addr() {
	printf '%s::%s/64' "$(tbv_link_prefix "$1" "$2")" "$(tbv_link_host "$1" "$2")"
}
