# Captured operation sequence: two-node NCCL hang on a dead TX ring

Everything here is measured, not inferred from reading the driver. Raw captures
are in `raw/`. The KUnit replay built from it is
`kernel/tests/tx_ring_stall_test.c`, driven by `tbv_test_path_tx_ring_stall()`
in `kernel/path.c`.

## Reproducers

| reproducer | what it drives | time to failure |
|---|---|---|
| `examples/kubernetes/2/nccl-hang-repro/job.yaml` (in forks-diffusion-pipe-prod) | 2-rank torch.distributed NCCL, `NCCL_DEBUG=INFO`, appmana-019 + appmana-008 | 3.5 s |
| `tools/tbv-hang-repro.sh` | bidirectional `ib_write_bw`, 1 MiB WRs, tx_depth 16-128, 4 QPs, appmana-021 + appmana-004 | 2 s |

Both replaced a training job that took tens of minutes to reach the same state
because it caches a 746-image dataset first. The NCCL reproducer never even
reaches its loop: the very first collective, the 4 KiB warmup `all_reduce`
(`SeqNum=1 OpType=ALLREDUCE NumelIn=1024`), is the one that dies.

`ib_write_bw` is the right stand-in for NCCL here because NCCL's IB net plugin
does not use SEND/RECV for bulk data -- it posts `IBV_WR_RDMA_WRITE` for the
payload, `IBV_WR_RDMA_WRITE_WITH_IMM` to signal, and a small `IBV_WR_RDMA_READ`
as a flush. The captured verb stream confirms the driver sees exactly that mix.

## Tooling that actually worked

- **NCCL layer**: `NCCL_DEBUG=INFO`, `NCCL_DEBUG_SUBSYS=INIT,NET,ENV`. Enough on
  its own to name the failing op and the CQE status. Nsight Systems was not
  installed on the chain hosts and was not needed.
- **Verbs layer**: bpftrace kprobes on the driver's own verb entry points, NOT
  an LD_PRELOAD shim -- `ibv_post_send()` is a `static inline` in `verbs.h` that
  dispatches through a provider function pointer, so `LD_PRELOAD` cannot
  intercept it. `tools/tbv-verb-trace.bt`. The module carries no BTF, so
  `ib_qp`/`ib_qp_attr` are read from vmlinux BTF by name and the WR structs at
  pahole-verified offsets.
- **Driver layer**: the module's own debugfs (`summary`, `peers`) sampled before
  and at the freeze by `tools/tbv-hang-repro.sh`, plus `dmesg`. The per-path
  counters are what localise the fault; the NCCL log alone points at the wrong
  node.

## The sequence

Verbatim from `raw/appmana-021.verbs.txt.gz`, one QP of four. Opcode 0 is
`IB_WR_RDMA_WRITE`; masks are `ib_qp_attr_mask`.

```
1  modify_qp qpn=2345 mask=0x39     state=1(INIT)
5  modify_qp qpn=2345 mask=0x129181 state=2(RTR) path_mtu=5 dest_qp=2335 rq_psn=931810 min_rnr=12
6  modify_qp qpn=2345 mask=0x12e01  state=3(RTS) sq_psn=2524616 timeout=14 retry_cnt=7 rnr_retry=7 max_rd_atomic=1
13 post_send qpn=2345
14 post_send_one op=0 wr_id=0 flags=0x2(SIGNALED) num_sge=1 len0=1048576 remote_addr=0x789584d68000 rkey=0xe
15 send_tx_done ctx=0xffff89793a950c00 status=0      <- 256 x 4 KiB frames per WR
...
```

Steady state on a healthy pair is 313 `post_send` / 313 `send_complete`. Under
load the tail is:

```
send_complete status=0     282
send_complete status=-110    4   <- ETIMEDOUT, surfaces as IBV_WC_RETRY_EXC_ERR
send_complete status=-125   27   <- ECANCELED, the flush of everything behind it
```

with `data_wr_retransmit` +810 and `data_wr_retry_exhausted` +4 across the run.

### Where it becomes unbounded

On appmana-019 the TX direction stopped completing entirely and stayed that way.
The driver's own counters, for **both** peers of that rail:

```
tx_poll enabled=1 calls=282365 completed=0
peer2 tx: enqueued=28 posted=2 completed=0
peer2 rx: data_rx_completed=432 data_rx_op_write=256   <- RX is healthy
```

So the fabric is up and frames arrive; only egress is dead. appmana-019 even
generated the ACKs (`data_tx_ack_ok=256`) -- they never left, because they use
the same path. Its dmesg carries the WR-level consequence:

```
thunderbolt_ibverbs: native TX stalled past ceiling, failing WR:
  qpn=0x915 dest_qp=0x946 psn=0 opcode=4 tx_pending=1 rail=0x0 route=0x3
```

and appmana-008 reports the peer view:

```
NET/IB: Got completion from peer 10.2.0.57<36708> with status=IBV_WC_RETRY_EXC_ERR(12)
  opcode=IBV_WC_RDMA_WRITE(1) vendor_err=0 hca usb4_rdma15
```

### The state at the freeze

1. `tbv_path_schedule_tx()` dequeues a data packet, hands its frame to
   `tb_ring_tx()`, increments `tx_inflight` and `data_tx_posted`.
2. The ring never completes it. `tbv_path_tx_poll_work()` runs forever:
   `tb_ring_poll()` returns NULL, `completed == 0`, and because
   `tx_inflight > 0` it re-arms itself unconditionally (path.c, the
   `if (atomic_read(&path->tx_inflight) > 0 || completed)` re-arm).
3. Nothing bounds that packet. The only ceiling,
   `tbv_path_expire_tx_data_queue()`, walks `tx_data_queue` -- and the packet
   left that queue in step 1. A **copied** packet, which is what the fleet uses
   (`data_wr_zcopy=0`, `data_wr_copied>0` in every capture), is not on
   `tx_zcopy_inflight` either. After the post it is referenced only by the ring
   frame, so `tbv_path_flush_tx_queue()` cannot see it either.
4. Its done callback therefore never runs. That callback is the only thing that
   drains the owning WR's `tx_pending`.
5. The WR dies only via the per-WR retransmit budget in ibdev.c, as
   `IBV_WC_RETRY_EXC_ERR`, which aborts the NCCL job. The path is never failed:
   it keeps advertising `active=1 data_ready=1`, so the next WR repeats this.

The `tbv_path_tx_stalled()` health gate already exists for this failure mode,
but it only excludes a rail from binding **new** QPs. It does nothing for frames
already in flight, and on a two-node segment there is no other rail to move to.

## What the KUnit replay asserts

`tbv_test_path_tx_ring_stall()` rebuilds exactly this: a real `tbv_path`, a real
running `tb_ring` that yields no completion through the real `tb_ring_poll()`, a
real packet enqueued through the real `tbv_path_send()` and then moved to the
posted state the way `tbv_path_schedule_tx()` leaves it. It runs the real
`tbv_path_tx_poll_work()` 60 times, aging the last-progress stamp 1 s per pass
against a 5 s ceiling, and asserts the owner is completed with `-ETIMEDOUT` and
`tx_inflight` recovers.

Three cases pass today and pin the scenario down (the poll really does run and
really does complete nothing; the health gate really does read stalled; the
ceiling is still disablable). Two fail, and they are the hang.

## Recommended fix (not applied here)

Track posted copied packets the way zero-copy ones are already tracked, and give
`tbv_path_tx_poll_work()` an escalation: when the ring has been polled with
`tx_inflight > 0` and zero completions for longer than `tx_queue_timeout_ms`,
release those packets with `-ETIMEDOUT` and mark the path not data-ready so the
QPs rebind or fail fast, instead of re-arming forever. Note also that the poll's
own stall warning back-dates `tx_last_progress_jiffies` every time it fires,
which is the only clock `tbv_path_tx_stalled()` consults -- an escalation keyed
off that stamp must not be reset by the warning that reports it.
