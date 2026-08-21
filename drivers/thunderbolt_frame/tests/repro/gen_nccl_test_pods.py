#!/usr/bin/env python3
"""Generate two privileged pods (rank0 on 020, rank1 on 009) that run a 2-rank
torch NCCL all_reduce over soft-RoCE rxe0, each wrapped in strace so we capture
the exact uverbs ioctl sequence NCCL issues. strace + nccl logs land on hostPath
/tmp/rxetrace so they survive pod exit/crash."""
import sys

IMAGE = "harbor.appmana.com/appmana/vllm-ampere:hotfix-idxfused-asyncstore-643d75e2a-20260611"
MASTER = "10.2.0.58"  # appmana-020 LAN ip

# minimal all_reduce; small tensor so the IB transport is exercised but fast
PYSCRIPT = (
    "import os,torch,torch.distributed as dist;"
    "dist.init_process_group('nccl');"
    "r=dist.get_rank();"
    "t=torch.ones(262144,device='cuda:0')*(r+1);"
    "dist.all_reduce(t);"
    "torch.cuda.synchronize();"
    "print('ALLREDUCE_OK rank',r,'val',t[0].item(),flush=True);"
    "dist.barrier();dist.destroy_process_group();print('DONE',r,flush=True)"
)

# strace: follow forks/threads, timestamp, capture verbs ioctls + which uverbs
# device is opened + signals (to catch SIGSEGV on the usb4_rdma run). Big -s so
# struct args aren't truncated.
def cmd(rank, dev):
    strace = (
        "strace -f -tt -s 512 -e trace=ioctl,openat,write "
        "-e signal=all -o /trace/strace.rank{r}.{d}.log".format(r=rank, d=dev)
    )
    nccl_env = (
        "export NCCL_DEBUG=INFO NCCL_DEBUG_SUBSYS=ALL "
        "NCCL_DEBUG_FILE=/trace/nccl.rank{r}.{d}.log;".format(r=rank, d=dev)
    )
    install = ("(command -v strace >/dev/null || "
               "(apt-get update -qq && apt-get install -y -qq strace)) "
               ">/trace/apt.rank{r}.{d}.log 2>&1; ".format(r=rank, d=dev))
    return (
        "set -x; " + install + nccl_env +
        " " + strace + " /usr/bin/python3 -c \"" + PYSCRIPT + "\" "
        "> /trace/stdout.rank{r}.{d}.log 2>&1; "
        "echo EXIT=$? >> /trace/stdout.rank{r}.{d}.log; sleep 3".format(r=rank, d=dev)
    )

def pod(name, node, rank, dev, hca):
    return {
        "apiVersion": "v1", "kind": "Pod",
        "metadata": {"name": name, "namespace": "default",
                     "labels": {"app": "rxetrace"}},
        "spec": {
            "restartPolicy": "Never",
            "hostNetwork": True,
            "nodeSelector": {"kubernetes.io/hostname": "appmana-" + node},
            "imagePullSecrets": [{"name": "harbor"}],
            "volumes": [
                {"name": "trace", "hostPath": {"path": "/tmp/rxetrace", "type": "Directory"}},
                {"name": "ib", "hostPath": {"path": "/dev/infiniband", "type": "Directory"}},
                {"name": "shm", "emptyDir": {"medium": "Memory", "sizeLimit": "2Gi"}},
                {"name": "prov", "hostPath": {"path": "/opt/tbchain/libusb4_rdma-rdmav34.so", "type": "File"}},
                {"name": "drv", "hostPath": {"path": "/opt/tbchain/usb4_rdma.driver", "type": "File"}},
            ],
            "containers": [{
                "name": "t", "image": IMAGE,
                "securityContext": {"privileged": True},
                "command": ["bash", "-lc", cmd(rank, dev)],
                "env": [
                    {"name": "RANK", "value": str(rank)},
                    {"name": "WORLD_SIZE", "value": "2"},
                    {"name": "MASTER_ADDR", "value": MASTER},
                    {"name": "MASTER_PORT", "value": "29555"},
                    {"name": "NCCL_SOCKET_IFNAME", "value": "eno1"},
                    {"name": "GLOO_SOCKET_IFNAME", "value": "eno1"},
                    {"name": "NCCL_IB_DISABLE", "value": "1" if dev == "sock" else "0"},
                    {"name": "NCCL_IB_HCA", "value": hca},
                    {"name": "NCCL_IB_GID_INDEX", "value": "1"},
                    {"name": "NCCL_NET_GDR_LEVEL", "value": "0"},
                    {"name": "NCCL_IB_TIMEOUT", "value": "22"},
                    # Production mitigation: force NCCL 2.30.4 (fixes the 2.28 GIN
                    # -1 deref). No NCCL_GIN_TYPE override -> tests the real config.
                    {"name": "LD_PRELOAD", "value": "/usr/lib/x86_64-linux-gnu/libnccl.so.2.30.4"},
                    {"name": "PYTHONFAULTHANDLER", "value": "1"},
                    {"name": "NVIDIA_VISIBLE_DEVICES", "value": "all"},
                ],
                "resources": {"limits": {"nvidia.com/gpu": "1"}},
                "volumeMounts": [
                    {"name": "trace", "mountPath": "/trace"},
                    {"name": "ib", "mountPath": "/dev/infiniband"},
                    {"name": "shm", "mountPath": "/dev/shm"},
                    {"name": "prov", "mountPath": "/usr/lib/x86_64-linux-gnu/libibverbs/libusb4_rdma-rdmav34.so"},
                    {"name": "drv", "mountPath": "/etc/libibverbs.d/usb4_rdma.driver"},
                ],
            }],
        },
    }

import json
dev = sys.argv[1] if len(sys.argv) > 1 else "rxe"
hca = "rxe0" if dev == "rxe" else "usb4_rdma0"
docs = [
    pod("rxetrace-r0-" + dev, "020", 0, dev, hca),
    pod("rxetrace-r1-" + dev, "009", 1, dev, hca),
]
print("\n---\n".join(json.dumps(d) for d in docs))
