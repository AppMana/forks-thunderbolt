#!/usr/bin/env bash
# RDMA latency/bandwidth benchmark between two thunderbolt_ibverbs chain nodes.
#
# Runs perftest (ib_send_lat / ib_send_bw) over the usb4_rdma rail: server on the
# first node, client on the second, OOB connection over the switched LAN
# (10.2.0.x), RDMA traffic over the Thunderbolt link. Reports the live
# native_data_e2e setting of each end so e2e=1 (hardware E2E flow control) and
# e2e=0 (software credits) can be compared apples-to-apples on the same pair.
#
# Latency (lat) is a single-in-flight ping-pong; bandwidth (bw) saturates the
# link with a tx window. Hardware E2E is a flow-control feature: it costs a
# little per-message latency but is meant to sustain throughput under load, so
# compare BOTH modes.
#
# Usage:
#   bench-rdma.sh <server-node> <client-node> <lat|bw> [iterations] [size-bytes]
# Examples:
#   bench-rdma.sh appmana-018 appmana-027 lat 5000 2
#   bench-rdma.sh appmana-018 appmana-027 bw  5000 65536
#
# Env: SSH_USER (default administrator), DOMAIN (default .i.appmana.com)
set -euo pipefail

SERVER="${1:?usage: $0 <server-node> <client-node> <lat|bw> [iterations] [size-bytes]}"
CLIENT="${2:?usage: $0 <server-node> <client-node> <lat|bw> [iterations] [size-bytes]}"
MODE="${3:-lat}"
ITERS="${4:-5000}"
case "$MODE" in
	lat) TOOL=ib_send_lat; SIZE="${5:-2}" ;;
	bw)  TOOL=ib_send_bw;  SIZE="${5:-65536}" ;;
	*) echo "ERROR: mode must be 'lat' or 'bw' (got '$MODE')" >&2; exit 2 ;;
esac
SSH_USER="${SSH_USER:-administrator}"
DOMAIN="${DOMAIN:-.i.appmana.com}"

ssh_node() { ssh -o BatchMode=yes -o ConnectTimeout=6 "${SSH_USER}@${1}${DOMAIN}" "$2"; }

server_oob=$(ssh_node "$SERVER" "ip -o -4 addr show eno1 | grep -oE '10[.]2[.]0[.][0-9]+' | head -1")
server_dev=$(ssh_node "$SERVER" "ls /sys/class/infiniband/ | grep '^usb4_rdma' | head -1")
client_dev=$(ssh_node "$CLIENT" "ls /sys/class/infiniband/ | grep '^usb4_rdma' | head -1")
server_e2e=$(ssh_node "$SERVER" "cat /sys/module/thunderbolt_ibverbs/parameters/native_data_e2e 2>/dev/null || echo '?'")
client_e2e=$(ssh_node "$CLIENT" "cat /sys/module/thunderbolt_ibverbs/parameters/native_data_e2e 2>/dev/null || echo '?'")

if [ -z "$server_dev" ] || [ -z "$client_dev" ]; then
	echo "ERROR: no usb4_rdma device on $SERVER ($server_dev) or $CLIENT ($client_dev) -- rail not up?" >&2
	exit 1
fi

echo "== $MODE: $SERVER (server, $server_dev, e2e=$server_e2e) <- $CLIENT (client, $client_dev, e2e=$client_e2e)"
echo "== iterations=$ITERS size=${SIZE}B oob=$server_oob"

ssh_node "$SERVER" "pkill -x $TOOL 2>/dev/null || true; sleep 1; \
	(setsid $TOOL -d '$server_dev' -F -n '$ITERS' -s '$SIZE' >/tmp/$TOOL.server 2>&1 &)"
sleep 2

out=$(ssh_node "$CLIENT" "$TOOL -d '$client_dev' -F -n '$ITERS' -s '$SIZE' '$server_oob' 2>&1" \
	| grep -E '^ *[0-9]+ +[0-9]+ +[0-9].*\.' | tail -1)
ssh_node "$SERVER" "pkill -x $TOOL 2>/dev/null || true"

if [ "$MODE" = lat ]; then
	echo "$out" | awk -v s="$server_e2e" '{printf "  e2e=%s  size=%sB  t_min=%s  t_typical=%s  t_avg=%s  p99=%s  p99.9=%s  (us)\n", s, $1, $3, $5, $6, $8, $9}'
else
	# ib_send_bw columns: #bytes #iters BW_peak[MB/s] BW_avg[MB/s] MsgRate[Mpps]
	echo "$out" | awk -v s="$server_e2e" '{printf "  e2e=%s  size=%sB  BW_peak=%s MB/s  BW_avg=%s MB/s  MsgRate=%s Mpps\n", s, $1, $3, $4, $5}'
fi
