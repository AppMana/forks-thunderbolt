#!/usr/bin/env bash
# Floor latency of the tbv RDMA path vs soft-RoCE, via perftest.
# Drives two nodes over SSH: server on $SRV_HOST (dev $SRV_DEV), client on
# $CLI_HOST (dev $CLI_DEV), out-of-band TCP to $SRV_OOB.
# Measures the "our driver + provider" floor with NO NCCL on top.
#
# Usage: floor-perftest.sh
# Env overrides: SRV_HOST CLI_HOST SRV_OOB SRV_DEV CLI_DEV GIDX N SIZES TESTS
set -u
SRV_HOST=${SRV_HOST:-appmana-019}
CLI_HOST=${CLI_HOST:-appmana-027}
SRV_OOB=${SRV_OOB:-10.2.0.57}
SRV_DEV=${SRV_DEV:-usb4_rdma5}     # 019 side of the 027<->019 tb link
CLI_DEV=${CLI_DEV:-usb4_rdma15}    # 027 side of the same link
GIDX=${GIDX:-1}
N=${N:-3000}
PORT=${PORT:-18522}
SIZES=${SIZES:-"64 4096 65536"}
TESTS=${TESTS:-"ib_write_lat ib_send_lat"}

run_one() {
  local test=$1 size=$2 sdev=$3 cdev=$4
  ssh "$SRV_HOST" "pkill -f '$test .*-p $PORT' 2>/dev/null; nohup $test -d $sdev -x $GIDX -s $size -n $N -p $PORT -F >/tmp/perf_srv.txt 2>&1 &"
  sleep 1.5
  ssh "$CLI_HOST" "$test -d $cdev -x $GIDX -s $size -n $N -p $PORT -F $SRV_OOB 2>&1" \
    | awk '/#bytes|t_min|[0-9]+ +[0-9]/{print}' | grep -E "^ *$size " | head -1
  sleep 0.5
}

for test in $TESTS; do
  echo "==== $test  dev(cli)=$CLI_DEV dev(srv)=$SRV_DEV gidx=$GIDX n=$N ===="
  echo "# bytes   t_min[us] t_max[us] t_typ[us]  t_avg[us]  t_stdev  99%[us]  99.9%[us]"
  for s in $SIZES; do
    printf '%-8s ' "$s"; run_one "$test" "$s" "$SRV_DEV" "$CLI_DEV"
  done
done
