#!/usr/bin/env bash
# Reproduce a high-concurrency bidirectional RDMA-write stall while preserving
# enough state to turn an observed failure into a deterministic regression
# test. This is a traffic/capture harness, not a recovery tool.
set -uo pipefail

usage() {
	cat <<'EOF'
Usage:
  tbv-hang-repro <server-ssh-host> <client-ssh-host> [output-directory]

The two hosts must be directly connected Thunderbolt/USB4 RDMA peers. The
harness selects the peer-facing usb4_rdma HCA on each host, snapshots driver
debugfs state, runs bidirectional ib_write_bw under a hard timeout, and writes
client/server output plus freeze-time captures into the output directory.

Exit status:
  0  all rounds completed
  1  preflight or capture failure
  3  workload reproduced a timeout/failure

Traffic environment:
  TBV_SIZE             payload bytes per WR                 (1048576)
  TBV_ITERS            WRs per direction per round          (200)
  TBV_TX_DEPTH         outstanding WRs per QP               (128)
  TBV_QPS              QPs per direction                    (32)
  TBV_ROUNDS           maximum rounds                       (3)
  TBV_ROUND_TIMEOUT    seconds allowed per round            (90)
  TBV_GID_INDEX        ib_write_bw -x GID index; required for GID-addressed
                       providers such as tbrxe              (unset = LID)

Address/device environment:
  TBV_SERVER_ADDR      client-reachable server bootstrap IP (auto-detected)
  TBV_DEV_SERVER       server usb4_rdma HCA                 (auto-detected)
  TBV_DEV_CLIENT       client usb4_rdma HCA                 (auto-detected)
  TBV_PEER_NAME_SERVER expected server-side tbr peer suffix (client shortname)
  TBV_PEER_NAME_CLIENT expected client-side tbr peer suffix (server shortname)
  TBV_TRACE_ROOT       default parent for captures          ($PWD/tbv-traces)

SSH environment:
  SSH_USER             prepended when a host has no user@   (current user)
  DOMAIN               appended to non-FQDN hostnames       (empty)

Remote requirements: bash/sh, coreutils, util-linux, sudo, iproute2, perftest
(ib_write_bw), the loaded thunderbolt_ibverbs module, mounted debugfs, and
passwordless SSH/sudo for the capture commands.
EOF
}

case "${1:-}" in
	-h|--help) usage; exit 0 ;;
esac

SERVER="${1:-}"
CLIENT="${2:-}"
[[ -n "$SERVER" && -n "$CLIENT" ]] || {
	usage >&2
	exit 1
}

# Provider generalization: MODULE names the kernel module whose srcversion
# and params are recorded (thunderbolt_ibverbs, tbrxe, rdma_rxe). DEV_*
# override device auto-detection (required for providers without per-link
# tbr-* netdevs). DEBUGFS=0 skips the legacy debugfs counter capture.
MODULE="${TBV_MODULE:-thunderbolt_ibverbs}"
DEV_SERVER="${TBV_DEV_SERVER:-}"
DEV_CLIENT="${TBV_DEV_CLIENT:-}"
DEBUGFS="${TBV_DEBUGFS:-1}"

SIZE="${TBV_SIZE:-1048576}"
ITERS="${TBV_ITERS:-200}"
TX_DEPTH="${TBV_TX_DEPTH:-128}"
QPS="${TBV_QPS:-32}"
ROUNDS="${TBV_ROUNDS:-3}"
ROUND_TIMEOUT="${TBV_ROUND_TIMEOUT:-90}"
# GID-addressed providers (tbrxe): perftest must use a GRH, which on an IB
# link-layer port means passing an explicit GID index. Empty = LID (legacy).
GID_INDEX="${TBV_GID_INDEX:-}"
SSH_USER="${SSH_USER:-${USER:-}}"
DOMAIN="${DOMAIN:-}"
if [[ -n "$DOMAIN" && "$DOMAIN" != .* ]]; then
	DOMAIN=".$DOMAIN"
fi
TRACE_ROOT="${TBV_TRACE_ROOT:-$PWD/tbv-traces}"
OUTDIR="${3:-$TRACE_ROOT/$(date +%Y%m%d-%H%M%S)-$(basename "$SERVER")-$(basename "$CLIENT")}"
RUN_ID="$(date +%Y%m%d%H%M%S)-$$"
REMOTE_SERVER_LOG="/tmp/tbv-hang-repro-$RUN_ID.server.log"
REMOTE_SERVER_PID="/tmp/tbv-hang-repro-$RUN_ID.server.pid"
REMOTE_SERVER_RC="/tmp/tbv-hang-repro-$RUN_ID.server.rc"
REMOTE_CLIENT_LOG="/tmp/tbv-hang-repro-$RUN_ID.client.log"
REMOTE_CLIENT_PID="/tmp/tbv-hang-repro-$RUN_ID.client.pid"
REMOTE_CLIENT_RC="/tmp/tbv-hang-repro-$RUN_ID.client.rc"

ssh_target() {
	local spec="$1" host user_prefix="" suffix="$DOMAIN"
	if [[ "$spec" == *@* ]]; then
		user_prefix="${spec%@*}@"
		host="${spec#*@}"
	else
		host="$spec"
		if [[ -n "$SSH_USER" ]]; then
			user_prefix="$SSH_USER@"
		fi
	fi
	[[ "$host" == *.* || "$host" == *:* ]] && suffix=""
	printf '%s%s%s\n' "$user_prefix" "$host" "$suffix"
}

ssh_node() {
	local node="$1" command="$2" wait_seconds="${3:-90}"
	timeout "$wait_seconds" ssh -o BatchMode=yes -o ConnectTimeout=6 \
		"$(ssh_target "$node")" "$command"
}

short_host() {
	local host="${1#*@}"
	host="${host%%.*}"
	printf '%s\n' "$host"
}

peer_key() {
	local key
	key="$(short_host "$1")"
	printf '%s\n' "${key//-/}"
}

for value in "$SIZE" "$ITERS" "$TX_DEPTH" "$QPS" "$ROUNDS" "$ROUND_TIMEOUT"; do
	[[ "$value" =~ ^[1-9][0-9]*$ ]] || {
		printf 'ERROR: traffic and timeout values must be positive integers\n' >&2
		exit 1
	}
done

for command in ssh timeout date seq; do
	command -v "$command" >/dev/null 2>&1 || {
		printf 'ERROR: required local command not found: %s\n' "$command" >&2
		exit 1
	}
done

mkdir -p "$OUTDIR" || exit 1
printf '== output %s\n' "$OUTDIR"

preflight() {
	local node="$1"
	local debugfs_check=true
	[[ "$DEBUGFS" == 1 ]] && debugfs_check="sudo -n test -r /sys/kernel/debug/thunderbolt_ibverbs/summary"
	ssh_node "$node" "command -v ib_write_bw >/dev/null && command -v ip >/dev/null && command -v setsid >/dev/null && command -v awk >/dev/null && test -r /sys/module/$MODULE/srcversion && $debugfs_check" 30
}

snapshot() {
	local node="$1" role="$2" tag="$3"
	{
		printf '### node=%s tag=%s t=%s\n' "$node" "$tag" "$(date -Is)"
		# The expansions below intentionally execute on the remote host.
		# shellcheck disable=SC2016
		ssh_node "$node" 'MOD='"$MODULE"'; echo "@@@HOST@@@"; printf "hostname=%s boot_id=%s uptime=%s kernel=%s loaded_version=%s loaded_src=%s\n" "$(hostname)" "$(cat /proc/sys/kernel/random/boot_id)" "$(cut -d. -f1 /proc/uptime)" "$(uname -r)" "$(modinfo -F version "$MOD" 2>/dev/null || echo none)" "$(cat /sys/module/"$MOD"/srcversion 2>/dev/null || echo none)"; echo "@@@PARAMS@@@"; for p in /sys/module/"$MOD"/parameters/*; do [ -r "$p" ] && printf "%s=%s\n" "${p##*/}" "$(cat "$p")"; done; echo "@@@SUMMARY@@@"; sudo -n cat /sys/kernel/debug/thunderbolt_ibverbs/summary 2>/dev/null || true; echo "@@@PEERS@@@"; sudo -n cat /sys/kernel/debug/thunderbolt_ibverbs/peers 2>/dev/null || true; echo "@@@DMESG@@@"; sudo -n dmesg | grep -E "tbframe|tbrxe" | tail -30 || true' 30
	} >> "$OUTDIR/counters-$role.txt" 2>&1
}

cleanup_remote() {
	local node="$1" pid_file="$2" log_file="$3" rc_file="$4"
	ssh_node "$node" "if read -r pid started < '$pid_file' 2>/dev/null; then case \"\$pid:\$started\" in *[!0-9:]*|:|*:) ;; *) current=\$(awk '{print \$22}' \"/proc/\$pid/stat\" 2>/dev/null || true); if [ \"\$current\" = \"\$started\" ]; then kill -TERM -- -\"\$pid\" 2>/dev/null || kill -TERM \"\$pid\" 2>/dev/null || true; sleep 1; current=\$(awk '{print \$22}' \"/proc/\$pid/stat\" 2>/dev/null || true); if [ \"\$current\" = \"\$started\" ]; then kill -KILL -- -\"\$pid\" 2>/dev/null || kill -KILL \"\$pid\" 2>/dev/null || true; fi; fi ;; esac; fi; rm -f '$pid_file' '$log_file' '$rc_file'" 30 >/dev/null 2>&1 || true
}

cleanup_all() {
	cleanup_remote "$SERVER" "$REMOTE_SERVER_PID" "$REMOTE_SERVER_LOG" "$REMOTE_SERVER_RC"
	cleanup_remote "$CLIENT" "$REMOTE_CLIENT_PID" "$REMOTE_CLIENT_LOG" "$REMOTE_CLIENT_RC"
}

on_signal() {
	trap - EXIT INT TERM HUP
	cleanup_all
	exit 130
}
trap cleanup_all EXIT
trap on_signal INT TERM HUP

if ! preflight "$SERVER" || ! preflight "$CLIENT"; then
	printf 'ERROR: remote preflight failed; see --help for requirements\n' >&2
	exit 1
fi

oob="${TBV_SERVER_ADDR:-}"
if [[ -z "$oob" ]]; then
	oob="$(ssh_node "$SERVER" "ip -o -4 addr show scope global | awk '\$2 !~ /^(tb-|tbr)/ {split(\$4,a,\"/\"); print a[1]; exit}'" 30)"
fi
[[ -n "$oob" ]] || {
	printf 'ERROR: no bootstrap IPv4 address found on %s; set TBV_SERVER_ADDR\n' "$SERVER" >&2
	exit 1
}

pick_dev() {
	local node="$1" expected_netdev="$2"
	ssh_node "$node" "expected='$expected_netdev'; for d in /sys/class/infiniband/usb4_rdma*; do [ -e \"\$d\" ] || continue; ndev=\$(cat \"\$d/ports/1/gid_attrs/ndevs/0\" 2>/dev/null || true); if [ \"\$ndev\" = \"\$expected\" ]; then basename \"\$d\"; fi; done" 30
}

dev_netdev() {
	local node="$1" dev="$2"
	ssh_node "$node" "cat /sys/class/infiniband/$dev/ports/1/gid_attrs/ndevs/0 2>/dev/null" 30
}

require_peer_dev() {
	local node="$1" dev="$2" expected="$3" actual
	actual="$(dev_netdev "$node" "$dev")"
	[[ "$actual" == "$expected" ]] || {
		printf 'ERROR: %s device %s belongs to %s, not %s\n' \
			"$node" "$dev" "${actual:-<none>}" "$expected" >&2
		return 1
	}
}

server_peer_name="${TBV_PEER_NAME_SERVER:-$(peer_key "$CLIENT")}"
client_peer_name="${TBV_PEER_NAME_CLIENT:-$(peer_key "$SERVER")}"
server_expected="tbr-$server_peer_name"
client_expected="tbr-$client_peer_name"

for value in "$server_expected" "$client_expected" "$oob"; do
	[[ "$value" =~ ^[A-Za-z0-9_.:%-]+$ ]] || {
		printf 'ERROR: unsafe peer or bootstrap value: %s\n' "$value" >&2
		exit 1
	}
done

SDEV="${TBV_DEV_SERVER:-$(pick_dev "$SERVER" "$server_expected")}"
CDEV="${TBV_DEV_CLIENT:-$(pick_dev "$CLIENT" "$client_expected")}"
[[ -n "$SDEV" && -n "$CDEV" ]] || {
	printf 'ERROR: no peer-facing usb4_rdma HCA (%s=%s %s=%s)\n' \
		"$SERVER" "$SDEV" "$CLIENT" "$CDEV" >&2
	exit 1
}
[[ "$(printf '%s\n' "$SDEV" | wc -l)" -eq 1 &&
	"$(printf '%s\n' "$CDEV" | wc -l)" -eq 1 ]] || {
	printf 'ERROR: ambiguous HCA selection; set TBV_DEV_SERVER/TBV_DEV_CLIENT\n' >&2
	exit 1
}

for value in "$SDEV" "$CDEV"; do
	[[ "$value" =~ ^[A-Za-z0-9_.:%-]+$ ]] || {
		printf 'ERROR: unsafe device value: %s\n' "$value" >&2
		exit 1
	}
done
# Explicit device overrides skip the tbr-* netdev binding check: providers
# other than the legacy driver (tbrxe, rdma_rxe) have no per-link netdev.
if [[ -z "$DEV_SERVER" ]]; then
	require_peer_dev "$SERVER" "$SDEV" "$server_expected" || exit 1
fi
if [[ -z "$DEV_CLIENT" ]]; then
	require_peer_dev "$CLIENT" "$CDEV" "$client_expected" || exit 1
fi

{
	printf 'started=%s\n' "$(date -Is)"
	printf 'server=%s target=%s device=%s expected_netdev=%s bootstrap=%s\n' \
		"$SERVER" "$(ssh_target "$SERVER")" "$SDEV" "$server_expected" "$oob"
	printf 'client=%s target=%s device=%s expected_netdev=%s\n' \
		"$CLIENT" "$(ssh_target "$CLIENT")" "$CDEV" "$client_expected"
	printf 'size=%s iters=%s tx_depth=%s qps=%s rounds=%s round_timeout=%s\n' \
		"$SIZE" "$ITERS" "$TX_DEPTH" "$QPS" "$ROUNDS" "$ROUND_TIMEOUT"
} > "$OUTDIR/manifest.txt"

printf '== %s(%s) <- %s(%s) bootstrap=%s size=%s tx_depth=%s qps=%s\n' \
	"$SERVER" "$SDEV" "$CLIENT" "$CDEV" "$oob" "$SIZE" "$TX_DEPTH" "$QPS"
if ! snapshot "$SERVER" server pre || ! snapshot "$CLIENT" client pre; then
	printf 'ERROR: initial state capture failed\n' >&2
	exit 1
fi

# Peak-bandwidth calculation is userspace post-processing. At high QP/WR
# counts it can outlive the timeout after transport has completed and look like
# a CQ hang, so retain average reporting and omit that diagnostic-only phase.
ARGS="-F -N -n $ITERS -s $SIZE -t $TX_DEPTH -q $QPS -b --report_gbits"
if [[ -n "$GID_INDEX" ]]; then
	ARGS="$ARGS -x $GID_INDEX"
fi

start_server() {
	ssh_node "$SERVER" "rm -f '$REMOTE_SERVER_LOG' '$REMOTE_SERVER_PID' '$REMOTE_SERVER_RC'; nohup setsid sh -c 'ib_write_bw -d $SDEV $ARGS; rc=\$?; printf \"%s\\n\" \"\$rc\" > \"$REMOTE_SERVER_RC\"; exit \"\$rc\"' > '$REMOTE_SERVER_LOG' 2>&1 < /dev/null & pid=\$!; started=\$(awk '{print \$22}' \"/proc/\$pid/stat\" 2>/dev/null || true); if [ -n \"\$started\" ]; then printf '%s %s\\n' \"\$pid\" \"\$started\" > '$REMOTE_SERVER_PID'; elif ! test -r '$REMOTE_SERVER_RC'; then exit 1; fi" 30
}

start_client() {
	ssh_node "$CLIENT" "rm -f '$REMOTE_CLIENT_LOG' '$REMOTE_CLIENT_PID' '$REMOTE_CLIENT_RC'; nohup setsid sh -c 'ib_write_bw -d $CDEV $ARGS $oob; rc=\$?; printf \"%s\\n\" \"\$rc\" > \"$REMOTE_CLIENT_RC\"; exit \"\$rc\"' > '$REMOTE_CLIENT_LOG' 2>&1 < /dev/null & pid=\$!; started=\$(awk '{print \$22}' \"/proc/\$pid/stat\" 2>/dev/null || true); if [ -n \"\$started\" ]; then printf '%s %s\\n' \"\$pid\" \"\$started\" > '$REMOTE_CLIENT_PID'; elif ! test -r '$REMOTE_CLIENT_RC'; then exit 1; fi" 30
}

wait_remote_result() {
	local node="$1" pid_file="$2" rc_file="$3" limit="$4"
	ssh_node "$node" "deadline=\$((\$(date +%s) + $limit)); while :; do if test -r '$rc_file'; then cat '$rc_file'; exit 0; fi; if ! read -r pid started < '$pid_file' 2>/dev/null; then exit 125; fi; current=\$(awk '{print \$22}' \"/proc/\$pid/stat\" 2>/dev/null || true); if [ \"\$current\" != \"\$started\" ]; then sleep 0.1; if test -r '$rc_file'; then cat '$rc_file'; exit 0; fi; exit 125; fi; if [ \"\$(date +%s)\" -ge \"\$deadline\" ]; then exit 124; fi; sleep 1; done" $((limit + 15))
}

append_remote_log() {
	local node="$1" role="$2" round="$3" result="$4" elapsed="$5" remote_log="$6"
	{
		printf '### round=%s result=%s elapsed=%ss\n' "$round" "$result" "$elapsed"
		ssh_node "$node" "cat '$remote_log' 2>/dev/null || true" 30 || \
			printf 'capture_error=remote log unavailable\n'
	} >> "$OUTDIR/$role.txt" 2>&1
}

decode_result() {
	local poll_rc="$1" output="$2"
	if [[ "$poll_rc" -eq 0 && "$output" =~ ^[0-9]+$ ]]; then
		printf '%s\n' "$output"
	else
		printf '%s\n' "$poll_rc"
	fi
}

failed_round=0
for round in $(seq 1 "$ROUNDS"); do
	printf '%s\n' "-- round $round"
	cleanup_all
	if ! start_server; then
		printf 'ERROR: could not start server workload in round %s\n' "$round" >&2
		append_remote_log "$SERVER" server "$round" start-failed 0 "$REMOTE_SERVER_LOG"
		exit 1
	fi
	sleep 2
	if ! start_client; then
		printf 'ERROR: could not start client workload in round %s\n' "$round" >&2
		append_remote_log "$SERVER" server "$round" client-start-failed 0 "$REMOTE_SERVER_LOG"
		append_remote_log "$CLIENT" client "$round" start-failed 0 "$REMOTE_CLIENT_LOG"
		exit 1
	fi

	start=$(date +%s)
	client_out=$(wait_remote_result "$CLIENT" "$REMOTE_CLIENT_PID" "$REMOTE_CLIENT_RC" "$ROUND_TIMEOUT")
	client_poll_rc=$?
	client_rc="$(decode_result "$client_poll_rc" "$client_out")"
	server_rc=running
	if [[ "$client_rc" -eq 0 ]]; then
		server_out=$(wait_remote_result "$SERVER" "$REMOTE_SERVER_PID" "$REMOTE_SERVER_RC" 10)
		server_poll_rc=$?
		server_rc="$(decode_result "$server_poll_rc" "$server_out")"
	fi
	elapsed=$(( $(date +%s) - start ))

	if [[ "$client_rc" -ne 0 || "$server_rc" != 0 ]]; then
		printf '!! round %s did not complete (client=%s server=%s elapsed=%ss) -- capturing live failure state\n' \
			"$round" "$client_rc" "$server_rc" "$elapsed"
		snapshot "$SERVER" server "hang-round$round" || true
		snapshot "$CLIENT" client "hang-round$round" || true
		for role in server client; do
			if [[ "$role" == server ]]; then node="$SERVER"; else node="$CLIENT"; fi
			ssh_node "$node" 'echo "## dmesg"; sudo -n dmesg | tail -200; echo "## blocked processes"; ps -eo pid,stat,wchan:32,comm | grep -E "ib_write_bw|tbv|D " || true' 40 \
				>> "$OUTDIR/hang-$role.txt" 2>&1
		done
		failed_round=$round
	fi

	append_remote_log "$SERVER" server "$round" "$server_rc" "$elapsed" "$REMOTE_SERVER_LOG"
	append_remote_log "$CLIENT" client "$round" "$client_rc" "$elapsed" "$REMOTE_CLIENT_LOG"
	cleanup_all
	[[ "$failed_round" -eq 0 ]] || break
done

cleanup_all
trap - EXIT INT TERM HUP
post_capture_ok=1
snapshot "$SERVER" server post || post_capture_ok=0
snapshot "$CLIENT" client post || post_capture_ok=0

if [[ "$failed_round" -ne 0 ]]; then
	printf '== FAILURE reproduced at round %s; captures in %s\n' "$failed_round" "$OUTDIR"
	exit 3
fi
if [[ "$post_capture_ok" -ne 1 ]]; then
	printf 'ERROR: workload completed but final state capture failed\n' >&2
	exit 1
fi
printf '== no hang in %s rounds; captures in %s\n' "$ROUNDS" "$OUTDIR"
