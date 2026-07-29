#!/usr/bin/env bash
# Smallest reproducer that drives the thunderbolt_ibverbs data path the way a
# two-rank NCCL training job does, and detects the hang instead of waiting for a
# human to notice it.
#
# NCCL's IB net plugin does not use SEND/RECV for bulk data. It posts
# IBV_WR_RDMA_WRITE for the payload, IBV_WR_RDMA_WRITE_WITH_IMM to signal the
# receiver, and a small IBV_WR_RDMA_READ as a flush. It keeps many WRs in
# flight per QP and runs both directions at once (ring all_reduce is inherently
# bidirectional). perftest's ib_write_bw with -b (bidirectional) and a deep tx
# window reproduces exactly that verb mix on the same driver paths, in seconds
# rather than the tens of minutes the real job needs to cache its dataset.
#
# The script is the harness, not the diagnosis: it snapshots the driver's own
# debugfs counters on both ends before and after, runs the traffic under a hard
# timeout, and on a stall dumps the freeze-time state (per-path credit window,
# tx queue depth, WR counters) which is what tbv-trace-to-kunit.py turns into a
# replay test. Everything lands in an output directory as plain text.
#
# Usage:
#   tbv-hang-repro.sh <server-node> <client-node> [outdir]
# Env:
#   TBV_SIZE       payload bytes per WR              (default 1048576)
#   TBV_ITERS      WRs per direction per round       (default 20000)
#   TBV_TX_DEPTH   outstanding WRs (NCCL-like)       (default 128)
#   TBV_QPS        QPs per direction                 (default 4)
#   TBV_ROUNDS     rounds to run before giving up    (default 40)
#   TBV_ROUND_TIMEOUT  seconds a round may take      (default 60)
#   TBV_DEV_SERVER / TBV_DEV_CLIENT  override rail selection
#   SSH_USER (default administrator), DOMAIN (default .i.appmana.com)
set -uo pipefail

SERVER="${1:?usage: $0 <server-node> <client-node> [outdir]}"
CLIENT="${2:?usage: $0 <server-node> <client-node> [outdir]}"
OUTDIR="${3:-$(git -C "$(dirname "$0")" rev-parse --show-toplevel)/drivers/thunderbolt_ibverbs/traces/$(date +%Y%m%d-%H%M%S)-$SERVER-$CLIENT}"

SIZE="${TBV_SIZE:-1048576}"
ITERS="${TBV_ITERS:-20000}"
TX_DEPTH="${TBV_TX_DEPTH:-128}"
QPS="${TBV_QPS:-4}"
ROUNDS="${TBV_ROUNDS:-40}"
ROUND_TIMEOUT="${TBV_ROUND_TIMEOUT:-60}"
SSH_USER="${SSH_USER:-administrator}"
DOMAIN="${DOMAIN:-.i.appmana.com}"

ssh_node() { timeout "${3:-90}" ssh -o BatchMode=yes -o ConnectTimeout=6 "${SSH_USER}@${1}${DOMAIN}" "$2"; }

mkdir -p "$OUTDIR"
echo "== output $OUTDIR"

# The driver's whole view of a rail, at one instant. Called before the run, and
# again the moment a round wedges: the diff between the two is the operation
# sequence the KUnit replay is built from.
snapshot() {
	local node="$1" tag="$2"
	{
		echo "### node=$node tag=$tag t=$(date -Is)"
		ssh_node "$node" 'sudo cat /sys/kernel/debug/thunderbolt_ibverbs/summary 2>/dev/null; echo "@@@PEERS@@@"; sudo cat /sys/kernel/debug/thunderbolt_ibverbs/peers 2>/dev/null' 30
	} >> "$OUTDIR/counters-$node.txt" 2>&1
}

oob=$(ssh_node "$SERVER" "ip -o -4 addr show | awk '\$4 ~ /^10[.]2[.]0[.]/ {split(\$4,a,\"/\"); print a[1]; exit}'" 30)
[ -n "$oob" ] || { echo "ERROR: no 10.2.0.x oob address on $SERVER" >&2; exit 1; }

pick_dev() { ssh_node "$1" "ls /sys/class/infiniband/ | grep '^usb4_rdma' | head -1" 30; }
SDEV="${TBV_DEV_SERVER:-$(pick_dev "$SERVER")}"
CDEV="${TBV_DEV_CLIENT:-$(pick_dev "$CLIENT")}"
[ -n "$SDEV" ] && [ -n "$CDEV" ] || { echo "ERROR: no usb4_rdma rail ($SERVER=$SDEV $CLIENT=$CDEV)" >&2; exit 1; }

echo "== $SERVER($SDEV) <- $CLIENT($CDEV) oob=$oob size=$SIZE tx_depth=$TX_DEPTH qps=$QPS"
snapshot "$SERVER" pre
snapshot "$CLIENT" pre

ARGS="-F -n $ITERS -s $SIZE -t $TX_DEPTH -q $QPS -b --report_gbits"

hung=0
for round in $(seq 1 "$ROUNDS"); do
	echo "-- round $round"
	ssh_node "$SERVER" "pkill -x ib_write_bw 2>/dev/null; sleep 1; (setsid ib_write_bw -d $SDEV $ARGS > /tmp/tbv-repro.srv 2>&1 &)" 30
	sleep 2
	start=$(date +%s)
	out=$(ssh_node "$CLIENT" "timeout $ROUND_TIMEOUT ib_write_bw -d $CDEV $ARGS $oob 2>&1" $((ROUND_TIMEOUT + 20)))
	rc=$?
	elapsed=$(( $(date +%s) - start ))
	{ echo "### round=$round rc=$rc elapsed=${elapsed}s"; echo "$out"; } >> "$OUTDIR/client.txt"

	# A round that ran out its timeout is the hang. A bidirectional
	# ib_write_bw of this size finishes in a couple of seconds; minutes is
	# never slowness, it is a stalled WR.
	if [ "$rc" -ne 0 ] || [ "$elapsed" -ge "$ROUND_TIMEOUT" ]; then
		echo "!! round $round did not complete (rc=$rc elapsed=${elapsed}s) -- capturing freeze state"
		snapshot "$SERVER" "hang-round$round"
		snapshot "$CLIENT" "hang-round$round"
		for n in "$SERVER" "$CLIENT"; do
			ssh_node "$n" 'echo "## dmesg"; sudo dmesg | tail -200; echo "## blocked tasks"; sudo cat /proc/*/stack 2>/dev/null | head -0; ps -eo pid,stat,wchan:32,comm | grep -E "ib_write_bw|tbv|D " ' 40 \
				>> "$OUTDIR/hang-$n.txt" 2>&1
		done
		hung=$round
		break
	fi
done

ssh_node "$SERVER" "pkill -x ib_write_bw 2>/dev/null; true" 30
ssh_node "$CLIENT" "pkill -x ib_write_bw 2>/dev/null; true" 30
snapshot "$SERVER" post
snapshot "$CLIENT" post

if [ "$hung" -ne 0 ]; then
	echo "== HUNG at round $hung; captures in $OUTDIR"
	exit 3
fi
echo "== no hang in $ROUNDS rounds; captures in $OUTDIR"
