#!/usr/bin/env bash
# Transport matrix for a single adjacent Thunderbolt link.
# Compares three software stacks over the SAME physical TB link:
#   1. usb4_rdma  (thunderbolt_frame_rxe RDMA, data over TB DMA rings)
#   2. soft-RoCE  (rxe on the thunderbolt_net tb-ch netdev -> RoCE over TB)
#   3. socket     (TCP over the thunderbolt_net tb-ch netdev, the NCCL socket path)
# Reports peak bandwidth and small-message half-RTT latency for each.
#
#   tb_transport_matrix.sh <server_node> <client_node> <link_label>
# Assumes: usb4_rdma0 up on both; tb-ch up with /31 IPs (.SVR / .CLI below);
# perftest + rdma-core present; iperf3 on at least the server.
set -uo pipefail
SVR="$1"; CLI="$2"; LABEL="${3:-link}"

# tb-ch /31 endpoints (server .even, client .odd) — discovered at runtime
SVR_TBIP=$(ssh "$SVR" 'ip -4 -o addr show | grep -oP "(?<=inet )10\.2\.1\.\d+(?=/31)" | head -1')
CLI_TBIP=$(ssh "$CLI" 'ip -4 -o addr show | grep -oP "(?<=inet )10\.2\.1\.\d+(?=/31)" | head -1')
SVR_IF=$(ssh "$SVR" 'ls /sys/class/net | grep "^tb-ch" | head -1')
CLI_IF=$(ssh "$CLI" 'ls /sys/class/net | grep "^tb-ch" | head -1')
echo "### $LABEL : server=$SVR($SVR_TBIP/$SVR_IF)  client=$CLI($CLI_TBIP/$CLI_IF)"

run_perf() {  # <dev> <svr_extra> -> peak BW (Gb/s) + lat (us)
  local dev="$1"
  ssh "$SVR" "pkill -9 ib_send_bw ib_send_lat 2>/dev/null; nohup ib_send_bw  -d $dev -F -s 1048576 -n 5000 >/tmp/sbw.log 2>&1 &" ; sleep 2
  local bw=$(ssh "$CLI" "ib_send_bw  -d usb4_rdma0 -F -s 1048576 -n 5000 $SVR_TBIP 2>/dev/null | awk '/1048576/{print \$4}'")
  ssh "$SVR" "pkill -9 ib_send_bw 2>/dev/null; nohup ib_send_lat -d $dev -F -s 8 -n 10000 >/tmp/slat.log 2>&1 &" ; sleep 2
  local lat=$(ssh "$CLI" "ib_send_lat -d usb4_rdma0 -F -s 8 -n 10000 $SVR_TBIP 2>/dev/null | awk '/^ 8 /{print \$6}'")
  echo "$bw $lat"
}

echo "-- 1. usb4_rdma (native ibverbs over TB rings) --"
ssh "$SVR" "pkill -9 ib_send_bw ib_send_lat 2>/dev/null; nohup ib_send_bw -d usb4_rdma0 -F -a -n 2000 >/tmp/s.log 2>&1 &"; sleep 2
ssh "$CLI" "ib_send_bw -d usb4_rdma0 -F -a -n 2000 $SVR_TBIP 2>/dev/null | awk 'NF>=4 && \$1+0>0 {print \"  bw \"\$1\"B -> \"\$4\" Gb/s\"}' | tail -6"
ssh "$SVR" "pkill -9 ib_send_bw 2>/dev/null; nohup ib_send_lat -d usb4_rdma0 -F -s 8 -n 10000 >/tmp/s.log 2>&1 &"; sleep 2
ssh "$CLI" "ib_send_lat -d usb4_rdma0 -F -s 8 -n 10000 $SVR_TBIP 2>/dev/null | tail -3"

echo "-- 2. soft-RoCE rxe on $SVR_IF/$CLI_IF (RoCE over thunderbolt_net) --"
ssh "$SVR" "sudo rdma link add rxe_tb type rxe netdev $SVR_IF 2>/dev/null; true"
ssh "$CLI" "sudo rdma link add rxe_tb type rxe netdev $CLI_IF 2>/dev/null; true"; sleep 1
ssh "$SVR" "pkill -9 ib_send_bw ib_send_lat 2>/dev/null; nohup ib_send_bw -d rxe_tb -F -a -n 2000 >/tmp/s.log 2>&1 &"; sleep 2
ssh "$CLI" "ib_send_bw -d rxe_tb -F -a -n 2000 $SVR_TBIP 2>/dev/null | awk 'NF>=4 && \$1+0>0 {print \"  bw \"\$1\"B -> \"\$4\" Gb/s\"}' | tail -6"
ssh "$SVR" "pkill -9 ib_send_bw 2>/dev/null; nohup ib_send_lat -d rxe_tb -F -s 8 -n 10000 >/tmp/s.log 2>&1 &"; sleep 2
ssh "$CLI" "ib_send_lat -d rxe_tb -F -s 8 -n 10000 $SVR_TBIP 2>/dev/null | tail -3"

echo "-- 3. socket TCP over $SVR_IF (NCCL socket path) --"
ssh "$SVR" "pkill -9 iperf3 2>/dev/null; nohup iperf3 -s -1 -B $SVR_TBIP >/tmp/ip.log 2>&1 &"; sleep 1
ssh "$CLI" "iperf3 -c $SVR_TBIP -t 5 -P 4 2>/dev/null | awk '/SUM/ && /receiver/{print \"  bw -> \"\$6\" \"\$7}'"
echo -n "  lat -> "; ssh "$CLI" "ping -c 20 -i 0.2 -q $SVR_TBIP 2>/dev/null | awk -F'/' '/rtt/{print \$5\" ms RTT (\"\$5/2\" ms half)\"}'"
echo "### end $LABEL"
