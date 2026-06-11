#!/usr/bin/env bash
# Repeatable thunderbolt-ibverbs bandwidth harness with counter-delta capture.
# Drives a server (S) + client (C) pair, snapshots the per-path debugfs
# counters around each run, and prints one clean result line per config.
#
# Usage: tbv-bench.sh <client_host> <server_host> <server_ip> <dev> "<modprobe args>" <label>
set -u
C=$1; S=$2; SIP=$3; DEV=$4; MARGS=$5; LABEL=$6
SSH="ssh -o ConnectTimeout=10 -o BatchMode=yes"

reload() { for h in $C $S; do $SSH administrator@$h "
  sudo pkill -9 ib_write_bw ib_send_bw 2>/dev/null
  sudo modprobe -r thunderbolt_ibverbs 2>/dev/null; sleep 1
  sudo modprobe thunderbolt_ibverbs $MARGS 2>&1 | head -1" >/dev/null 2>&1; done
  # wait for the device + data_ready on both
  for h in $C $S; do for i in $(seq 1 20); do
    $SSH administrator@$h "ls /sys/class/infiniband 2>/dev/null | grep -q $DEV && sudo grep -q data_ready=1 /sys/kernel/debug/thunderbolt_ibverbs/peers" 2>/dev/null && break; sleep 1
  done; done
}

snap() { $SSH administrator@$1 "sudo cat /sys/kernel/debug/thunderbolt_ibverbs/summary 2>/dev/null | grep -E 'data_wr_send:|data_wr_copied:|data_wr_zcopy:|data_tx_credit_stalls' " 2>/dev/null | tr '\n' ' '; }
cstall() { $SSH administrator@$1 "sudo cat /sys/kernel/debug/thunderbolt_ibverbs/peers 2>/dev/null | grep -oE 'data_tx_credit_stalls=[0-9]+' | head -1 | cut -d= -f2" 2>/dev/null; }

run() { # $1=mode(uni|duplex) $2=n
  local mode=$1 n=$2 bflag=""; [ "$mode" = duplex ] && bflag="-b"
  $SSH administrator@$S "sudo pkill -9 ib_write_bw 2>/dev/null; sleep 1; ib_write_bw -d $DEV -x 1 -c RC $bflag -n $n --report_gbits > /tmp/bench_s.log 2>&1 &" >/dev/null 2>&1
  sleep 8
  local cs0=$(cstall $S)
  $SSH administrator@$C "timeout 50 ib_write_bw -d $DEV -x 1 -c RC $bflag -n $n --report_gbits $SIP 2>&1 | grep -E '^ 65536' | tail -1" 2>/dev/null
  local cs1=$(cstall $S)
  $SSH administrator@$S "sudo pkill -9 ib_write_bw 2>/dev/null" >/dev/null 2>&1
  echo "    credit_stall_delta=$(( ${cs1:-0} - ${cs0:-0} ))"
}

echo "### $LABEL  [$MARGS]"
reload
echo "  uni:    $(run uni 5000)"
echo "  duplex: $(run duplex 20000)"
