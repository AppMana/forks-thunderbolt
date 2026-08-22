#!/usr/bin/env bash
# Build nccl_mock on both nodes and sweep the QP-count / opcode / device matrix
# to find which dimension flips a passing single-QP run into NCCL's
# IBV_WC_RETRY_EXC_ERR. Server runs on SRV (009), client on CLI (020).
#
# usage: ./run_sweep.sh            # full matrix
#        ./run_sweep.sh quick      # just q=1 and q=6 on usb4_rdma0
set -u
SRV_NODE=${SRV_NODE:-appmana-009}
CLI_NODE=${CLI_NODE:-appmana-020}
SRV_IP=${SRV_IP:-10.2.0.15}        # 009 LAN ip (OOB + RoCE)
SSH="ssh -o LogLevel=ERROR"
SRC=/tmp/nccl_mock.c
BIN=/tmp/nccl_mock

echo "== deploy + build =="
for n in "$SRV_NODE" "$CLI_NODE"; do
  scp -q "$(dirname "$0")/nccl_mock.c" "administrator@$n:$SRC"
  $SSH "administrator@$n" "gcc -O2 -o $BIN $SRC -libverbs && echo $n built" 2>&1 | grep -v Warning
done

run_one() {  # dev nqps op size
  local dev=$1 q=$2 op=$3 sz=$4
  printf '\n--- dev=%s qps=%s op=%s size=%s ---\n' "$dev" "$q" "$op" "$sz"
  $SSH "administrator@$SRV_NODE" \
    "timeout 40 $BIN -d $dev -g 1 -q $q -o $op -s $sz -n 64 -x 8" \
    >/tmp/srv.out 2>&1 &
  local srvpid=$!
  sleep 1
  $SSH "administrator@$CLI_NODE" \
    "timeout 40 $BIN -d $dev -g 1 -q $q -o $op -s $sz -n 64 -x 8 -c $SRV_IP" \
    2>&1 | grep -vE "Warning" | sed 's/^/  client: /'
  wait $srvpid 2>/dev/null
  $SSH "administrator@$SRV_NODE" "cat /tmp/srv.out 2>/dev/null" 2>&1 | grep -vE "Warning" | sed 's/^/  server: /'
}

if [ "${1:-}" = "quick" ]; then
  run_one usb4_rdma0 1 write 65536
  run_one usb4_rdma0 6 write 65536
  exit 0
fi

echo "== control: rxe0 (soft-RoCE, expected to pass everywhere) =="
for q in 1 6; do run_one rxe0 $q write 65536; done

echo "== usb4_rdma0 QP-count sweep =="
for q in 1 2 4 6; do run_one usb4_rdma0 $q write 65536; done

echo "== usb4_rdma0 opcode sweep (q=6) =="
for op in write write_imm send; do run_one usb4_rdma0 6 $op 65536; done

echo "== usb4_rdma0 size sweep (q=1, isolate size vs concurrency) =="
for sz in 4096 65536 1048576; do run_one usb4_rdma0 1 write $sz; done
