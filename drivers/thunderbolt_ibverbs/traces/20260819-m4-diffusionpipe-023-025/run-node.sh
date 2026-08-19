#!/bin/bash
# M4 gate: two-rank DiffusionPipe PP=2 over tbrxe (bare-metal containerd, no k8s).
# Usage: run-node.sh <node_rank>
set -euo pipefail
RANK="${1:?rank}"
IMG=ghcr.io/appmana/forks-diffusion-pipe-prod:2b23812
NAME=dp-pp2-gate
sudo k0s ctr -n k8s.io run --rm --net-host \
  --runc-binary /usr/local/nvidia/toolkit/nvidia-container-runtime \
  --env NVIDIA_VISIBLE_DEVICES=all \
  --env NVIDIA_DRIVER_CAPABILITIES=compute,utility \
  --cap-add CAP_IPC_LOCK \
  --device /dev/infiniband \
  --mount type=bind,src=/var/lib/diffusion-pipe/hf,dst=/cache/huggingface,options=rbind:rw \
  --mount type=bind,src=/var/lib/diffusion-pipe/cache,dst=/cache/diffusion-pipe,options=rbind:rw \
  --mount type=bind,src=/var/lib/diffusion-pipe/outputs,dst=/outputs,options=rbind:rw \
  --mount type=bind,src=/var/lib/diffusion-pipe/m4-run,dst=/work,options=rbind:ro \
  --mount type=bind,src=/usr/lib/x86_64-linux-gnu/libibverbs/libtbrxe-rdmav34.so,dst=/usr/lib/x86_64-linux-gnu/libibverbs/libtbrxe-rdmav34.so,options=rbind:ro \
  --mount type=bind,src=/etc/libibverbs.d/tbrxe.driver,dst=/etc/libibverbs.d/tbrxe.driver,options=rbind:ro \
  --mount type=bind,src=/dev/shm,dst=/dev/shm,options=rbind:rw \
  --env HF_HOME=/cache/huggingface \
  --env DIFFUSION_PIPE_NUM_NODES=2 \
  --env DIFFUSION_PIPE_NODE_RANK="$RANK" \
  --env DIFFUSION_PIPE_NODE_IPS=10.2.0.61,10.2.0.67 \
  --env DIFFUSION_PIPE_MASTER_ADDR=10.2.0.61 \
  --env DIFFUSION_PIPE_MASTER_PORT=29517 \
  --env NCCL_DEBUG=INFO \
  --env NCCL_DEBUG_SUBSYS=INIT,NET \
  --env NCCL_IB_DISABLE=0 \
  --env NCCL_IB_HCA=usb4_rdma \
  --env NCCL_IB_ADDR_FAMILY=AF_INET6 \
  --env "NCCL_IB_SUBNET_AWARE_ROUTING=prefer_hca[usb4_rdma\\d*]" \
  --env NCCL_IB_MERGE_NICS=0 \
  --env NCCL_NET_MERGE_LEVEL=LOC \
  --env NCCL_NET_GDR_LEVEL=0 \
  --env NCCL_ALGO=Ring \
  --env NCCL_PROTO=Simple \
  --env NCCL_SOCKET_IFNAME=enp110s0 \
  --env GLOO_SOCKET_IFNAME=enp110s0 \
  "$IMG" "$NAME" \
  bash -lc 'ulimit -l unlimited || true; ibv_devices; exec diffusion-pipe launch --config /work/config.toml'
