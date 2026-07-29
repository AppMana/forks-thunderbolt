# Debugging thunderbolt_ibverbs in the kernel

The tools available for this driver, what each one actually proves, and what it
costs. Ordered by how quickly it discriminates between causes, not by
sophistication.

Two lessons paid for in this driver already, before any tool:

- **Confirm a counter means what you think.** `/sys/kernel/debug/thunderbolt_ibverbs/peers`
  prints **one block per peer path**. Reading the idle block produced a
  confident report that the driver had posted nothing while it was posting
  90,000 frames on the other path.
- **A stall detector can lie in both directions.** The `tx stall` warning fired
  every five seconds on a healthy ring because only the poll stamped the
  progress field, while interrupts reaped 99.5% of completions. Check whether
  `posted` and `completed` are BOTH advancing across consecutive warnings
  before believing a frame is stuck.

## What the fleet kernel already has

`6.17.0-40-generic` ships with, verified in `/boot/config-$(uname -r)`:

| option | state |
|---|---|
| `CONFIG_DYNAMIC_DEBUG` | y |
| `CONFIG_FUNCTION_TRACER` / `CONFIG_FUNCTION_GRAPH_TRACER` | y |
| `CONFIG_KPROBES` / `CONFIG_KPROBE_EVENTS` | y |
| `CONFIG_DEBUG_INFO_BTF` | y |
| `CONFIG_FTRACE` | y |
| `CONFIG_MAGIC_SYSRQ` | y |
| `CONFIG_KASAN` / `CONFIG_KCSAN` / `CONFIG_PROVE_LOCKING` / `CONFIG_DMA_API_DEBUG` | **not set** |

So everything below marked *no rebuild* works on any chain host right now.
Everything marked *rebuild* needs a debug kernel, which must be added as an
extra boot entry and never made the default.

## Tier 1 — no rebuild, seconds to minutes

### `rdma` (userspace, instant)

Answers "whose QPs are these and what state are they in" before any kernel
work. Essential when a production consumer shares the rails.

```bash
rdma resource show qp link usb4_rdma15   # lqpn/rqpn, state, pid, comm
rdma link show                            # ACTIVE/LINK_UP + the netdev, which
                                          # names the peer: tbr-appmana004
rdma statistic show link usb4_rdma15
```

`pid`/`comm` is how you tell your own traffic from a live DSV4 rank. A
measurement that cannot attribute its QPs is not a measurement.

### The driver's own debugfs

```bash
cat /sys/kernel/debug/thunderbolt_ibverbs/summary   # global counters
cat /sys/kernel/debug/thunderbolt_ibverbs/peers     # PER PATH, read every block
```

Key counters and what a bad value means:

| counter | reading |
|---|---|
| `data_tx_posted` vs `data_tx_completed` | a frozen `completed` with `posted` climbing is a genuinely stuck ring |
| `data_wr_retry_exhausted` / `data_wr_timeout` | WRs failing as `IBV_WC_RETRY_EXC_ERR` |
| `data_wr_no_capacity` | internal frame budget rejecting a post; distinct from `data_wr_no_path` |
| `data_rx_crc_error*` | link quality, not software |
| `tx_credits=n/max` | software credit window; full while stalled means credits are not the constraint |
| `data_rx_no_qp` | frames arriving for a QP that does not exist here — misrouting |

### Dynamic debug (`no rebuild`)

`CONFIG_DYNAMIC_DEBUG=y`, so every `pr_debug` is switchable at runtime with no
rebuild. debugfs may need mounting first.

```bash
mount -t debugfs none /sys/kernel/debug 2>/dev/null
echo 'module thunderbolt_ibverbs +pfl' > /sys/kernel/debug/dynamic_debug/control
echo 'file path.c line 2300-2500 +p'   > /sys/kernel/debug/dynamic_debug/control
```

The driver is thin on `pr_debug` sites — **adding them is the intended use**,
not a workaround. A `pr_debug` at each branch of the post path costs nothing
when disabled.

### ftrace function_graph (`no rebuild`)

The fastest way to see where a call returns early, without editing code.
`funcgraph-retval` prints return values, which usually answers "which function
produced the error" outright.

```bash
cd /sys/kernel/tracing
echo 0 > tracing_on
echo function_graph > current_tracer
echo 'tbv_*' > set_ftrace_filter
echo funcgraph-retval > trace_options       # return values
echo 100 > tracing_thresh                    # only calls slower than 100us
echo tbv_post_send > set_graph_function
echo 1 > tracing_on; sleep 5; echo 0 > tracing_on
cat trace
```

`tracing_thresh` is the trick for a stall: it hides the fast healthy calls and
leaves only the ones that blocked.

### kprobes / perf probe (`no rebuild`)

For arguments and `$retval` on specific functions once function_graph has
narrowed it down.

```bash
perf probe -m thunderbolt_ibverbs 'tbv_path_schedule_tx path'
perf probe -m thunderbolt_ibverbs 'tb_ring_tx%return ret=$retval'
perf record -e probe:* -aR -- sleep 5 && perf script
```

### bpftrace (`no rebuild`)

Already the workhorse here. **Read the existing scripts before writing new
ones**: `tools/tbv-ring-progress.bt` (posts vs completions split by reaping
context), `tools/tbv-post-trace.bt`, `tools/tbv-verb-trace.bt`,
`tools/tbv-send-timeline.bt`.

The module has no BTF, so struct access uses vmlinux types by name or
pahole-verified offsets. `ibv_post_send` is a `static inline` in `verbs.h`
dispatching through a provider pointer — **LD_PRELOAD does not intercept it**;
probe the kernel entry points instead.

### Magic SysRq (`no rebuild`)

`kernel.sysrq=176` is already set. Useful when userspace is starved but the
kernel still schedules:

```bash
echo t > /proc/sysrq-trigger   # all task stacks
echo w > /proc/sysrq-trigger   # blocked (D-state) tasks only
echo l > /proc/sysrq-trigger   # backtrace of all active CPUs
```

Output goes to dmesg, which Promtail ships to Loki within ~100 ms — so it
survives a subsequent wedge. See `docs/cluster.md` for querying.

## Tier 2 — rebuild required, hours

Add as an extra GRUB entry; never displace the stock kernel. A slow debug
kernel can trip the 120 s systemd watchdog, so raise or disable
`RuntimeWatchdogSec` on those hosts for the duration and restore it after.

### `CONFIG_DMA_API_DEBUG` — highest value for this bug class

Validates every DMA mapping and sync against the kernel's own bookkeeping:
missing or wrong-direction `dma_sync_single_for_device`/`_for_cpu`, double
map, use-after-unmap, mapping crossing a boundary. **A descriptor whose buffer
was never synced to the device is exactly what "hardware never completed it"
looks like.** Boot with `dma_debug_driver=thunderbolt` to scope it.

Cheap relative to KASAN and directly targets the DMA hypothesis.

### `CONFIG_KCSAN` — for the concurrency hypothesis

The driver both feeds and polls the same ring from several contexts, and the
failure is concurrency dependent (one QP is fine at 16.63 Gb/s; 32 QPs
bidirectional fails in seconds). KCSAN finds data races that lockdep cannot —
lockdep only reasons about lock *ordering*, not unprotected shared state.

### `CONFIG_KASAN` / `CONFIG_KFENCE`

KASAN for descriptor or context corruption; KFENCE is the low-overhead
sampling alternative that can run on a production-ish host. **Do not enable
KASAN and KCSAN together** when chasing a timing-dependent bug — the combined
slowdown changes the timing enough to hide it.

### `CONFIG_PROVE_LOCKING` (lockdep)

Already wired into the unit tests: `KUNIT_LOCKDEP=1 tools/run-kunit.sh` builds a
kernel with `PROVE_LOCKING`, `DEBUG_MUTEXES`, `DEBUG_ATOMIC_SLEEP` and runs the
suite under it. On hardware it additionally covers paths the tests never reach
(`peer.control_lock`, `link.lock`, `session.lock`).

Validate the harness before trusting a clean run: plant a deliberate A→B/B→A
and confirm the splat appears.

### kgdb / kdb / kgdboe

Live inspection — breakpoint at the stall and read ring head/tail and
descriptor flags directly. Needs `CONFIG_KGDB` plus a usable console; assess
serial availability per host before planning around it. `kgdboe` over the LAN
is possible but fragile. Usually a last resort after tracing.

### kdump / crash

`crashkernel=` is **already on the fleet cmdline**, so a forced dump is nearly
free:

```bash
echo c > /proc/sysrq-trigger        # panic -> kdump
crash /usr/lib/debug/boot/vmlinux-$(uname -r) /var/crash/*/dump.*
```

Gives whole-memory state at the freeze — every ring, every queue, every
context — which live tracing cannot. The natural next step when you know
*which* structure is wrong but not why.

## Tier 3 — hardware, when software is exonerated

If the descriptor is genuinely never completed by the device, no software
tracing will find it. Rule the hardware in or out:

```bash
# Thunderbolt core state, tunnels and paths
ls /sys/kernel/debug/thunderbolt/
# PCIe advanced error reporting
dmesg | grep -iE 'AER|PCIe Bus Error|Corrected|Uncorrected'
lspci -vvv -s <nhi-bdf> | grep -A12 'Advanced Error'
# link-level errors the driver already counts
grep data_rx_crc_error /sys/kernel/debug/thunderbolt_ibverbs/summary
```

The NHI ring producer/consumer registers for the stuck hop, plus the peer's RX
ring occupancy, are what distinguish "TX engine backpressured by the wire
because the receiver is not draining" from "TX engine stopped locally". That
distinction decides whether hardware end-to-end flow control (`RING_FLAG_E2E`)
is the answer — see the note below before touching it.

A non-trivial `data_rx_crc_error` rate means marginal signal integrity; reseat
or swap the cable on the affected link before spending days in software.

## Static analysis (no hardware at all)

```bash
tools/run-kunit.sh                       # 188 unit tests
KUNIT_LOCKDEP=1 tools/run-kunit.sh       # same, under lockdep
spatch --sp-file tools/rx_lock_reentry.cocci --dir kernel/   # bug-pattern sweep
python3 tools/lock-graph.py              # transitive lock closure
make C=2 CHECK=sparse                    # context imbalance on _locked helpers
```

`tools/tbv-trace-to-kunit.py` converts a captured trace into a KUnit skeleton,
which is how a field hang becomes a regression test.

## Do not enable RING_FLAG_E2E casually

Hardware end-to-end flow control needs **both** ends to negotiate it — `tbnet`
exchanges `TBNET_E2E` before setting the flag. The tbv native wire protocol has
no such bit, so enabling it unilaterally means one side backpressures while an
older peer overruns its ring. `nhi.c` also rewrites `e2e_tx_hop` under
`QUIRK_E2E`. The safe increment is a HELLO capability bit, enabled only when
both ends advertise it, A/B tested on one link pair.

## Rules when using live hosts

- The chain hosts self-recover: an armed SP5100 watchdog resets a wedged node
  in ~3 minutes. Wedging a node during a debug run is acceptable.
- **Never drain, reboot or power off a node by hand** — use `graceful-drain.sh`
  then `graceful-uncordon.sh`. See the cardinal rule in `docs/cluster.md`.
- Never kill processes holding `/dev/infiniband/uverbs*`; that kills live
  inference ranks.
- Take a shared workload off the chain through GitOps (`replicas: 0` in the
  LeaderWorkerSet), not `kubectl scale`, which Flux reverts.
- Leave every host with a loaded module whose `srcversion` matches the `.ko` on
  disk, and rails present in `/sys/class/infiniband`.
