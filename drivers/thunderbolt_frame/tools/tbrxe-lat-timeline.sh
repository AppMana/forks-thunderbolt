#!/usr/bin/env bash
# tbrxe-lat-timeline.sh - stage visibility for the thunderbolt_frame_rxe /
# thunderbolt_frame data path.
#
# Default (aggregate) mode prints per-interval stage counts, cheap enough to
# leave attached under a 32-QP storm without deciding the outcome:
#
#   17:24:08 xmit=40233 enq=40233 irq=1201 work=2402 txc=40230 clirx=40219 \
#   admit=81201/75489 nospc_ctrl=0 nospc_data=0
#   17:24:08 engine post=12 send=81344 recv=40219 cq=25 retx=3 rnrt=0
#
#   admit=<calls>/<refused>; nospc_* = tbframe_alloc_frame -ENOSPC by class.
#   A stalled link shows xmit/txc/clirx collapsing to ~0 while admit stays
#   refused-heavy (window pinned) or retx ticks (timer-paced retries only).
#
# --events emits the per-event lines tbrxe-lat-analyze.py consumes (one line
# per probe fire -- ib_send_lat ping-pong scale ONLY).
#
# Why two probe mechanisms: rxe_post_send, rxe_sender, rxe_receiver,
# rxe_cq_post, retransmit_timer and rnr_nak_timer exist in BOTH rdma_rxe and
# thunderbolt_frame_rxe, so a plain bpftrace kprobe cannot attach ("symbol
# is not unique"),
# and perf refuses BPF programs on trace_kprobe events, so bpftrace cannot
# ride a module-qualified tracefs kprobe either. Those six therefore run as
# tracefs kprobe events (module:symbol form): aggregate mode samples their
# hit counts from kprobe_profile, events mode streams trace_pipe (mono
# clock, matching bpftrace's nsecs) through an awk converter. Symbols unique
# to thunderbolt_frame / thunderbolt_frame_rxe stay plain bpftrace kprobes.
#
# Usage: sudo tbrxe-lat-timeline.sh [--events] [interval-seconds]
set -euo pipefail

MODE=agg
INTERVAL=1
for a in "$@"; do
	case "$a" in
	--events) MODE=events ;;
	[0-9]*) INTERVAL="$a" ;;
	*) echo "usage: $0 [--events] [interval-seconds]" >&2; exit 1 ;;
	esac
done

[ "$(id -u)" = 0 ] || { echo "must run as root" >&2; exit 1; }

TRACEFS=/sys/kernel/tracing
KPE="$TRACEFS/kprobe_events"

# name:module:symbol -> analyzer event tag
EVENTS="
tbvlt_post:thunderbolt_frame_rxe:rxe_post_send:POST
tbvlt_send:thunderbolt_frame_rxe:rxe_sender:SEND
tbvlt_recv:thunderbolt_frame_rxe:rxe_receiver:RECV
tbvlt_cq:thunderbolt_frame_rxe:rxe_cq_post:CQ
tbvlt_retx:thunderbolt_frame_rxe:retransmit_timer:RETX
tbvlt_rnrt:thunderbolt_frame_rxe:rnr_nak_timer:RNRT
"

PIDS=""
cleanup() {
	[ -n "$PIDS" ] && kill $PIDS 2>/dev/null || true
	for spec in $EVENTS; do
		name="${spec%%:*}"
		echo 0 > "$TRACEFS/events/kprobes/$name/enable" 2>/dev/null || true
		echo "-:kprobes/$name" >> "$KPE" 2>/dev/null || true
	done
}
trap cleanup EXIT INT TERM

cleanup 2>/dev/null	# clear leftovers from a previous aborted run
for spec in $EVENTS; do
	name="${spec%%:*}"
	rest="${spec#*:}"		# module:symbol:TAG
	sym="${rest%:*}"		# module:symbol
	echo "p:kprobes/$name $sym" >> "$KPE"
	echo 1 > "$TRACEFS/events/kprobes/$name/enable"
done

if [ "$MODE" = events ]; then
	# bpftrace nsecs is CLOCK_MONOTONIC; align the trace clock with it.
	old_clock=$(sed -n 's/.*\[\(.*\)\].*/\1/p' "$TRACEFS/trace_clock")
	echo mono > "$TRACEFS/trace_clock"
	echo > "$TRACEFS/trace"

	# trace_pipe: "comm-pid [cpu] flags timestamp: event_name: (addr)"
	# -> "<ns> TAG cpu=<n>" (the tbrxe-lat-analyze.py line shape).
	awk '
	/tbvlt_/ {
		for (i = 1; i <= NF; i++) {
			if ($i ~ /^\[[0-9]+\]$/) { cpu = substr($i, 2, length($i) - 2) + 0 }
			if ($i ~ /^[0-9]+\.[0-9]+:$/) { ts = $i; sub(/:$/, "", ts) }
			if ($i ~ /^tbvlt_/) { ev = $i; sub(/:$/, "", ev) }
		}
		split(ts, t, ".")
		ns = t[1] * 1000000000 + t[2] * 1000
		tag = ""
		if (ev == "tbvlt_post") tag = "POST"
		else if (ev == "tbvlt_send") tag = "SEND"
		else if (ev == "tbvlt_recv") tag = "RECV"
		else if (ev == "tbvlt_cq") tag = "CQ"
		else if (ev == "tbvlt_retx") tag = "RETX"
		else if (ev == "tbvlt_rnrt") tag = "RNRT"
		if (tag != "") { printf "%d %s cpu=%d\n", ns, tag, cpu; fflush() }
	}' < "$TRACEFS/trace_pipe" &
	PIDS="$!"

	bpftrace -e '
kprobe:tbframe_xmit
{
	$d = *(uint64 *)arg1;          /* frame->data */
	$op = *(uint8 *)$d;
	$p2 = (uint32)(*(uint8 *)($d + 9));
	$p1 = (uint32)(*(uint8 *)($d + 10));
	$p0 = (uint32)(*(uint8 *)($d + 11));
	printf("%llu XMIT cpu=%d op=%x psn=%u\n", nsecs, cpu,
	       $op, ($p2 << 16) | ($p1 << 8) | $p0);
}
kprobe:__tb_ring_enqueue
	{ printf("%llu ENQ cpu=%d ring=%llx\n", nsecs, cpu, (uint64)arg0); }
kprobe:ring_msix
	{ printf("%llu IRQ cpu=%d ring=%llx\n", nsecs, cpu, (uint64)arg1); }
kprobe:ring_work
	{ printf("%llu WORK cpu=%d work=%llx\n", nsecs, cpu, (uint64)arg0); }
kprobe:tbframe_core_tx_complete
	{ printf("%llu TXC cpu=%d\n", nsecs, cpu); }
kprobe:tbrxe_client_rx
{
	$d = *(uint64 *)arg2;          /* frame->data */
	$op = *(uint8 *)$d;
	$p2 = (uint32)(*(uint8 *)($d + 9));
	$p1 = (uint32)(*(uint8 *)($d + 10));
	$p0 = (uint32)(*(uint8 *)($d + 11));
	printf("%llu CLIRX cpu=%d op=%x psn=%u\n", nsecs, cpu,
	       $op, ($p2 << 16) | ($p1 << 8) | $p0);
}' || true

	echo "$old_clock" > "$TRACEFS/trace_clock" 2>/dev/null || true
	exit 0
fi

# Aggregate mode: engine-side counts from kprobe_profile deltas, one line
# per interval, alongside the bpftrace interval line below.
(
	declare -A prev
	while sleep "$INTERVAL"; do
		line="$(date +%H:%M:%S) engine"
		while read -r name hits _; do
			case "$name" in tbvlt_*) ;; *) continue ;; esac
			d=$((hits - ${prev[$name]:-0}))
			prev[$name]=$hits
			case "$name" in
			tbvlt_post) line="$line post=$d" ;;
			tbvlt_send) line="$line send=$d" ;;
			tbvlt_recv) line="$line recv=$d" ;;
			tbvlt_cq)   line="$line cq=$d" ;;
			tbvlt_retx) line="$line retx=$d" ;;
			tbvlt_rnrt) line="$line rnrt=$d" ;;
			esac
		done < "$TRACEFS/kprobe_profile"
		echo "$line"
	done
) &
PIDS="$!"

bpftrace -e '
kprobe:tbframe_xmit           { @xmit++; }
kprobe:__tb_ring_enqueue      { @enq++; }
kprobe:ring_msix              { @irq++; }
kprobe:ring_work              { @work++; }
kprobe:tbframe_core_tx_complete { @txc++; }
kprobe:tbrxe_client_rx        { @clirx++; }
kprobe:tbrxe_admit            { @admit++; }
kretprobe:tbrxe_admit /retval == 0/ { @admit_ref++; }
kprobe:tbframe_alloc_frame    { @is_ctrl[tid] = arg2; }
kretprobe:tbframe_alloc_frame /(int64)retval == -28/ {
	if (@is_ctrl[tid]) { @nospc_ctrl++; }
	else { @nospc_data++; }
}
kretprobe:tbframe_alloc_frame { delete(@is_ctrl[tid]); }
interval:s:'"$INTERVAL"' {
	time("%H:%M:%S ");
	printf("xmit=%d enq=%d irq=%d work=%d txc=%d clirx=%d admit=%d/%d nospc_ctrl=%d nospc_data=%d\n",
	       (int64)@xmit, (int64)@enq, (int64)@irq, (int64)@work,
	       (int64)@txc, (int64)@clirx, (int64)@admit, (int64)@admit_ref,
	       (int64)@nospc_ctrl, (int64)@nospc_data);
	clear(@xmit); clear(@enq); clear(@irq); clear(@work);
	clear(@txc); clear(@clirx); clear(@admit); clear(@admit_ref);
	clear(@nospc_ctrl); clear(@nospc_data);
}
END { clear(@is_ctrl); }' || true
