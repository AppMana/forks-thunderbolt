# DSV4-over-ibverbs NCCL failure: trace, diagnosis, and the mux design

Date: 2026-06-16. Context: after rolling thunderbolt-ibverbs 0.2.9 chain-wide, DSV4
(PP=12, TP=1) over the `usb4_rdma` transport (`NCCL_IB_DISABLE=0`) crash-loops at
NCCL init. This documents exactly how it was traced and what the root cause is,
so the topology-aware NCCL net plugin (`libnccl-net-tbchain`, task #36) is built
against evidence rather than a guess.

## Symptom

`kubectl get lws` shows the leader stuck `0/1`; the group recreates every ~3–4 min
(`restartPolicy: RecreateGroupOnPodRestart`), `restartCount=0` on each fresh pod.
Without NCCL debug the only signal is the Python traceback tail:

    PyNcclCommunicator.__init__ -> self.all_reduce(data) -> ncclAllReduce
      -> RuntimeError: NCCL error: unhandled system error

## How it was traced

1. Confirmed it is **not** the 0.2.9 control fix: all 22 XDomain rails are
   `negotiated=1 ready_sent=1 remote_ready=1` after the roll, and a control
   re-negotiation hang would surface as a *timeout* minutes later, not an
   immediate transport error ~16 s into worker init.
2. Confirmed the GID table on a node is healthy (eui64 link-local, v4-mapped, and
   a `fd5a::` ULA, all on the switched-LAN roce_netdev) — so it is **not** the
   boot-DHCP GID race (`feedback_nccl_gid_inet6`).
3. Enabled `NCCL_DEBUG=INFO` + `NCCL_DEBUG_SUBSYS=INIT,NET,GRAPH,TUNING` on the
   LWS (kept `NCCL_IB_DISABLE=0`), reconciled, and **poll-snapshotted** the leader
   log (the `-f` stream races the group recreation; a snapshot loop that saves the
   log once it contains both `NCCL INFO` and the crash captures one whole attempt).
   Captured 3520 lines / 2338 `NCCL INFO` lines of one failing attempt.

## Root cause (from the trace)

NCCL selects the Thunderbolt RDMA device on every rank:

    NET/IB : Using [0]usb4_rdma0:1/RoCE [RO]; OOB eno1:10.2.0.x   (bootstrap over switched LAN)
    Using network IB

It then builds **rings and trees across all 12 ranks**. The ring is the chain
order (`... -> 10 -> 11 -> 0 -> 1 -> ...`), and the **tree** adds non-adjacent
parent/child links, e.g. from appmana-004 (rank 11):

    Channel 03/0 : 11[0] -> 3[0] [send]    via NET/IB/0
    Channel 03/0 : 3[0] -> 11[0] [receive] via NET/IB/0

Rank 11 (appmana-004) and rank 3 (appmana-019) are **not physical Thunderbolt
neighbours**. When NCCL programs that QP, the INIT→RTR transition fails:

    misc/ibvwrap.cc:309 NCCL WARN Call to ibv_modify_qp failed with 101
      Network is unreachable, on dev usb4_rdma0:1, curr state INIT, next state RTR,
      local GID index 2, local GID  fd5a:8000:1:0:7285:c2ff:fedc:3559,
                         remote GID fd5a:8000:1:0:5a11:22ff:feb7:7543
    transport/net_ib/connect.cc:429 -> 2    (ncclSystemError)
    ... -> enqueue.cc:3053 -> 2

`errno 101 ENETUNREACH` at `ibv_modify_qp(RTR)` is the kernel saying it has **no
path to the remote GID**: `usb4_rdma` only carries RDMA to a node's *physical TB
neighbours*; a non-adjacent peer's GID is unreachable. The same WARN fires on
several ranks (020→a non-neighbour, 004→a non-neighbour) — every non-adjacent
ring-wrap/tree edge. The first all-reduce that touches such an edge returns
`unhandled system error`, EngineCore aborts, the group recreates, loop.

`NCCL_ALGO=Tree,Ring` means **both** topologies are built, so even eliminating the
ring wrap would leave the tree's non-adjacent edges failing. The fundamental
mismatch: NCCL assumes any rank can open a QP to any other rank, but a linear TB
chain only provides RDMA between adjacent ranks.

Note (line 118): `ncclOsDlopen(libnccl-net.so) failed: cannot open shared object
file` — NCCL looks for an external net plugin, finds none, and falls back to the
built-in `NET/IB`. **That dlopen is the hook the mux plugs into.**

## Why the previous serve "worked"

The 2026-06-13 serve (`dsv4-ibverbs-first-serve`) dodged this with a fork gate
`VLLM_PYNCCL_SKIP_WARMUP=1` (skip the constructor all-reduce that forces the
12-wide ring) plus `cpu_object_fanout` for PP tokens (so the 12-wide NCCL group is
never actually used at run time — PP is point-to-point adjacent). That **avoids**
the cross-chain collective rather than making IB span the chain; any real
collective would still hit ENETUNREACH. The mux is the proper fix.

## The mux: `libnccl-net-tbchain`

An external NCCL net plugin (the `libnccl-net.so` NCCL tried to dlopen) that
implements `ncclNet_v*` and routes **per connection, by peer adjacency**:

- **Adjacent peer** (physical TB neighbour): use `usb4_rdma` IB verbs — full
  Thunderbolt bandwidth. This is the only path the decode hot loop uses (PP
  hand-off rank `i -> i+1` is always adjacent), so single-user latency is
  unaffected.
- **Non-adjacent peer** (ring wrap, tree edges): route over the switched-LAN
  RoCE/sockets on the roce_netdev (enp38s0/eno1, 10.2.0.0/24). Off the hot path —
  only init-time and non-adjacent collective edges pay the LAN cost.

Adjacency is known to the thunderbolt-ibverbs driver (each peer's route/rail), and
to the plugin from the chain order (`tb_chain_order`) plus the GID→node map. The
plugin presents one logical net device to NCCL; `connect()`/`accept()` inspect the
remote handle's GID, classify adjacent vs not, and dispatch to the IB backend or
the socket/RoCE backend. `isend`/`irecv`/`test` forward to the chosen backend.

Result: NCCL's ring and tree both connect (no ENETUNREACH), IB stays enabled, and
the latency-critical adjacent hops keep full TB bandwidth.
