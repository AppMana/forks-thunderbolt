#!/usr/bin/env bash
# Verify the 020<->009 usb4_rdma pair end-to-end with the v2 module:
#  1. counter-delta ib_write_bw (requester must see data_rx_ack + ack_matched)
#  2. nccl_mock q=1 and q=6 (NCCL's RC/RDMA_WRITE pattern)
set -u
KEYS='data_tx_posted:|data_rx_op_write:|data_tx_ack_ok:|data_rx_ack:|data_rx_ack_matched:|data_rx_duplicate_ack:|data_wr_retry_exhausted:'
snap() { ssh administrator@appmana-$1 "sudo grep -hE '$KEYS' /sys/kernel/debug/thunderbolt_ibverbs/summary" 2>/dev/null | grep -v Warning | tr '\n' ' '; echo; }

echo "=== BEFORE 020: $(snap 020)"
echo "=== BEFORE 009: $(snap 009)"

ssh administrator@appmana-009 'timeout 20 ib_write_bw -d usb4_rdma0 -x 1 -F --report_gbits -s 65536 -n 500 2>&1' >/tmp/vp_srv 2>&1 &
sleep 2
ssh administrator@appmana-020 "timeout 20 ib_write_bw -d usb4_rdma0 -x 1 -F --report_gbits -s 65536 -n 500 10.2.0.15 2>&1" >/tmp/vp_cli 2>&1
wait 2>/dev/null

echo "=== perftest client result ==="
grep -E "65536|BW|error|Failed|Completion" /tmp/vp_cli | grep -v Warning | tail -4
echo "=== AFTER 020:  $(snap 020)"
echo "=== AFTER 009:  $(snap 009)"

echo
echo "=== nccl_mock q=1 ==="
ssh administrator@appmana-009 "timeout 15 /tmp/nccl_mock -d usb4_rdma0 -g 1 -q 1 -o write -s 65536 -n 32 -x 8" >/tmp/vp_m1s 2>&1 &
sleep 1
ssh administrator@appmana-020 "timeout 15 /tmp/nccl_mock -d usb4_rdma0 -g 1 -q 1 -o write -s 65536 -n 32 -x 8 -c 10.2.0.15" 2>&1 | grep -vE "Warning|heartbeat" | grep -E "OK|FAIL|TIMEOUT"
wait 2>/dev/null

echo "=== nccl_mock q=6 (NCCL channel count) ==="
ssh administrator@appmana-009 "timeout 15 /tmp/nccl_mock -d usb4_rdma0 -g 1 -q 6 -o write -s 65536 -n 32 -x 8" >/tmp/vp_m6s 2>&1 &
sleep 1
ssh administrator@appmana-020 "timeout 15 /tmp/nccl_mock -d usb4_rdma0 -g 1 -q 6 -o write -s 65536 -n 32 -x 8 -c 10.2.0.15" 2>&1 | grep -vE "Warning|heartbeat" | grep -E "OK|FAIL|TIMEOUT"
wait 2>/dev/null

echo
echo "=== dmesg guardrails fired? (both nodes) ==="
for n in 020 009; do
  echo "--- $n ---"
  ssh administrator@appmana-$n 'sudo dmesg | grep -E "rebound|retry exhausted|unknown QPN|wire v" | tail -4' 2>&1 | grep -v Warning
done
