# debugging a thunderbolt nhi tx ring descriptor that never completes

## the failure

A descriptor handed to a Thunderbolt NHI DMA TX ring is never completed by the
hardware. The `RING_DESC_COMPLETED` flag never appears, so `tbv_path_tx_complete()`
never runs, `tx_inflight` never decrements, and everything queued behind it stalls.

It is concurrency dependent. One QP unidirectional sustains 16.63 Gb/s on the same
link. 32 QPs bidirectional at 256 KiB wedges in seconds. Reproduced on
appmana-025 <-> appmana-023 with

```
TBV_QPS=32 TBV_SIZE=262144 tools/tbv-hang-repro.sh appmana-025 appmana-023
```

which hung at round 1 after 13 s.

The freeze capture is the thing to read before picking any tool. On appmana-023,
peer 2:

```
data_tx_enqueued=126307 data_tx_posted=53843 data_tx_completed=53811
```

53843 - 53811 = **32**, which is exactly `data_tx_max_inflight=32`. The path is not
missing one descriptor and limping. It has hit its inflight ceiling with 32
descriptors the ring accepted and never completed, so it stops posting entirely and
the 72k enqueued frames behind them go nowhere. On appmana-025 the same instant
shows `data_tx_enqueued=95353 data_tx_posted=29084 data_tx_completed=29084` with
`tx_credits=768/768` — full credits, nothing in flight, and still 66k frames stuck
in the software queue.

Those two shapes are different and both need explaining. That distinction drives the
priority order below.

## the finding that reorders everything

Before reaching for a sanitizer, read `ring_write_descriptors()` in
`drivers/thunderbolt/nhi.c:253-276`:

```c
descriptor = &ring->descriptors[ring->head];
descriptor->phys = frame->buffer_phy;
descriptor->time = 0;
descriptor->flags = RING_DESC_POSTED | RING_DESC_INTERRUPT;
if (ring->is_tx) {
        descriptor->length = frame->size;
        descriptor->eof = frame->eof;
        descriptor->sof = frame->sof;
}
ring->head = (ring->head + 1) % ring->size;
if (ring->is_tx)
        ring_iowrite_prod(ring, ring->head);
```

Two things are wrong with this, and both were confirmed by direct inspection rather
than inferred:

**1. There is no barrier of any kind in nhi.c.** `grep -nE '\bwmb\(|\brmb\(|\bmb\(\)|dma_wmb|dma_rmb|smp_wmb|smp_rmb|barrier\(|_relaxed' nhi.c` returns nothing at all.
The descriptor lives in `dma_alloc_coherent()` memory (nhi.c:678-680) and is filled
with plain stores; the doorbell is a bare `iowrite32()` (`ring_iowrite_prod()`,
nhi.c:206-220). Nothing orders the descriptor stores against the doorbell write.
`ring->lock` is held, but a spinlock orders CPUs against each other, not CPU stores
against a device's view of coherent memory.

The kernel's own rule for this is explicit — from
`Documentation/core-api/wrappers/memory-barriers.rst`, a driver sharing descriptor
memory with a device must issue `dma_wmb()` before updating the ownership/valid bit
so the descriptor contents are visible before the device can act on the flag, then
`writel()` to ring the doorbell.

This exact bug is a recurring one in-tree, which is good evidence it is real and not
theoretical:

- `mmc: cqhci: commit descriptors before setting the doorbell` — a write barrier
  before the doorbell fixed intermittent ADMA errors
  (https://lkml.iu.edu/hypermail/linux/kernel/1910.1/06233.html)
- `scsi: ufs: commit descriptors before setting the doorbell`
  (https://lkml.iu.edu/hypermail/linux/kernel/1508.3/03124.html)
- `dma: fsl-edma: Add write barrier after TCD descriptor fill` — the same bug class
  again in 2025 (https://lkml.iu.edu/2511.3/03097.html)

**2. The TX field write order is inverted.** `struct ring_desc` (nhi_regs.h:28-35)
packs `length:12, eof:4, sof:4, flags:12` into one 32-bit word. The code writes
`flags` — carrying `RING_DESC_POSTED` — *before* `length`/`eof`/`sof`. So the POSTED
bit is live in that word from the first store onward, and the subsequent
`length/eof/sof` writes are a read-modify-write of the word that already advertises
the descriptor as posted. There is no instant at which the code expresses "contents
complete, now publish". The standard pattern is the exact opposite: fill payload,
barrier, then set the valid bit.

Matt Keeter's writeup of an STM32H7 ethernet DMA bug is the best public account of
what this looks like from the outside: hardware read descriptor words non-atomically,
so an ownership bit could be observed valid while another field was still stale.
His conclusion is directly applicable — barriers between descriptor field writes are
necessary but not sufficient; the doorbell protocol has to guarantee hardware only
advances into a descriptor the driver has explicitly finished
(https://www.mattkeeter.com/blog/2023-10-31-dma/).

**Why this explains the concurrency dependence.** One QP unidirectional posts
descriptors with large gaps and little contention on the coherent descriptor
cacheline. 32 QPs bidirectional hammer the same ring from many cores with the
completion path concurrently reading the same lines. That is precisely the regime in
which a store that is architecturally ordered only by program order, with no barrier
and no ordering against an MMIO write, can be observed out of order by the device.

**The rate is consistent with this and inconsistent with a counting bug.** 451,771 is
not a multiple of the ring size (1024, `TBV_NATIVE_RING_SIZE`), of the Apple ring size
(16384), or of 65536. `451771 mod 1024 = 187`. There is no wrap or counter-width
boundary at that number. A roughly 1-in-450k failure is the signature of a narrow
race window, not a modulus error.

## the second finding, which fits the observed signature better

The barrier bug above is real and worth fixing, but read the archived stall trace
closely (`traces/20260729-clean-019-027/instrumented-019-008.txt`) and it does not
actually fit:

```
tx stall route=0x3 rail=0x0 inflight=1 posted=451771 completed=451770
tx stall route=0x3 rail=0x0 inflight=1 posted=451776 completed=451775
tx stall route=0x3 rail=0x0 inflight=1 posted=451781 completed=451780
```

`posted` and `completed` are **both advancing**, in lockstep, with the gap pinned at
exactly 1. The ring has not stopped. It is completing frames normally, and has been
for another ten frames after the "stall" began.

That rules out a hardware descriptor loss, by the ring's own semantics. `ring_work()`
(nhi.c:305-318) walks from `ring->tail` and `break`s at the first descriptor without
`RING_DESC_COMPLETED`. Completion is strictly in order. If the hardware had genuinely
never completed descriptor N, `tail` would be stuck at N and **nothing behind it could
ever complete** — `data_tx_completed` would freeze. It does not freeze. So the
hardware did complete that descriptor.

The completion was lost in software. And there is an exact mechanism for it in the
code.

`ring_work()` invokes the completion callback like this (nhi.c:331-333):

```c
list_del_init(&frame->list);
if (frame->callback)
        frame->callback(ring, frame, canceled);
```

**A frame whose `callback` is NULL is silently dropped.** It is removed from
`ring->in_flight`, and nothing is notified. No warning, no counter, no error path.

Now look at what `tbv_path_tx_complete()` does to a frame it is finishing with
(path.c:984-998), under `tx_lock`:

```c
f->packet = NULL;
f->done = NULL;
f->done_ctx = NULL;
f->frame.callback = NULL;          /* <-- */
...
list_del_init(&f->free_node);
list_add_tail(&f->free_node, &path->tx_free);
```

It clears `frame.callback` and returns the frame to the free pool. Meanwhile
`atomic_dec(&path->tx_inflight)` at path.c:1025 is unconditional — every invocation
of the callback decrements it — and `atomic_inc(&path->tx_inflight)` at path.c:2457
happens once per frame before posting.

So `tx_inflight` pinned at exactly 1 forever means **exactly one frame was
incremented and posted, and its callback never ran**. Given the ring drops
NULL-callback frames without a word, the candidate mechanism is a frame that reaches
`ring_work()` with `frame.callback` already NULL — a frame recycled through the free
list and re-posted while the ring still held a reference to it, or completed twice.

This is a *frame lifecycle race between two independent lock domains*, which is the
structural risk flagged earlier: `ring->lock` in nhi.c and `path->tx_lock` in path.c
protect two different lists (`ring->in_flight` versus `path->tx_free` /
`path->tx_frame_inflight`) that stay consistent only by the assumption that
`frame->callback` fires exactly once per frame. Nothing enforces that assumption, and
nhi.c's silent NULL check is precisely the place where a violation becomes invisible
instead of loud.

It also explains the concurrency dependence at least as well as the barrier
hypothesis — a recycle race needs two contexts hitting the same frame, which is what
32 QPs bidirectional provides and 1 QP unidirectional does not.

**The cheapest possible test, and it needs no debug kernel:** change nhi.c's silent
drop into a loud one.

```c
list_del_init(&frame->list);
if (frame->callback)
        frame->callback(ring, frame, canceled);
else
        WARN_ONCE(1, "tb_ring: frame %p completed with NULL callback, hop %d tx %d\n",
                  frame, ring->hop, ring->is_tx);
```

If that WARN fires at the moment `tx_inflight` sticks, the mechanism is proven and
the remaining work is entirely in path.c's frame lifecycle. If it never fires, this
hypothesis is dead and the barrier hypothesis moves back to the front. Either way it
is one line and one module rebuild.

**Note the two captures are not the same shape.** The archived 019-008 trace shows
`inflight=1` with the ring still running. The fresh appmana-025/023 capture taken for
this document shows `data_tx_posted - data_tx_completed == 32`, which is exactly
`data_tx_max_inflight`, with 72k frames queued behind and the ring not draining at
all. The second is what a genuinely stopped ring looks like. These may be two
different bugs, or the same leak repeated 32 times until the ceiling is hit. Deciding
which is the first question the tracepoints in P2 should answer, and it is the reason
P1 (counter shape over time) is ranked as highly as it is.

## the honest caveat that shapes the whole tool list

**None of KASAN, KCSAN, or `CONFIG_DMA_API_DEBUG` will detect a missing `dma_wmb()`
before an MMIO doorbell.**

- `DMA_API_DEBUG` validates the *map/unmap/sync bookkeeping* of the DMA API. It has
  no model of the device and no model of store ordering. Its Kconfig text is precise
  about scope: it detects "common bugs in device drivers like double-freeing of DMA
  mappings or freeing mappings that were never allocated".
- KASAN detects invalid *memory accesses*. A correctly-addressed store in the wrong
  order is not an invalid access.
- KCSAN detects races between *CPU threads*. It has no notion of a device as a
  concurrent agent. `CONFIG_KCSAN_WEAK_MEMORY` does model store buffering and does
  flag missing `smp_wmb()`/release barriers — but only where a second *kernel*
  context races the first. The device reading `ring->descriptors[]` is not a kernel
  context, so KCSAN cannot see it.

I could not find any upstream commit or bug describing a descriptor written back
with done=0 forever, nor an explicit head/tail race writeup in nhi.c. The
best-evidenced adjacent bug class upstream is interrupt masking races around
polling, e.g. `thunderbolt: Mask ring interrupt properly when polling starts`, where
commit 4ffe722eefcb inverted `__ring_interrupt_mask(ring, false)` when it should have
been `true` (https://lkml.iu.edu/hypermail/linux/kernel/1712.0/00296.html), and the
`ring_interrupt_active()` WARN regressions
(https://bugzilla.redhat.com/show_bug.cgi?id=1703369). Be sceptical of any claim
that a "lost completion" commit exists upstream — I looked and did not find one.

So the highest-value action is **not** a sanitizer. It is a controlled experiment:
add `dma_wmb()` before the doorbell and reorder the TX field writes, then run the
reproducer. That is a two-line change, costs one build, and is decisive in either
direction. Everything below exists to attack the hypotheses that survive if that
experiment does not fix it.

## prioritised methodology

### P0a — the NULL-callback WARN (no debug kernel needed)

**What it proves.** Whether the lost completion is nhi.c silently dropping a frame
whose `callback` is NULL. See the section above for the full argument and the patch.
This is ranked first because it is one line, it directly targets the mechanism that
best fits the archived signature, and it is decisive in both directions.

**Cost.** One DKMS rebuild of thunderbolt-tbfix. Zero runtime cost.

**Risk to a live host.** None. `WARN_ONCE` on a path that currently does nothing.

### P0b — the barrier experiment (no debug kernel needed)

**What it proves.** Whether the stall is CPU/device store ordering. If adding
`dma_wmb()` between the descriptor stores and `ring_iowrite_prod()`, and writing
`length/eof/sof` before `flags`, makes 32-QP bidirectional survive where it
previously died in 13 s, the mechanism is established.

**Cost.** One DKMS rebuild of thunderbolt-tbfix. No kernel rebuild. Seconds of
runtime per trial.

**Risk to a live host.** None beyond a module reload. `dma_wmb()` on x86 is a
compiler barrier and is free.

**Caveat.** A negative result is weaker than a positive one. A barrier changes
timing, so "it stopped reproducing" is not proof by itself — run long enough to beat
the ~450k-frame mean time to failure by a good margin, and confirm the counters show
sustained `data_tx_posted == data_tx_completed` progress rather than merely a longer
gap before the wedge.

### P1 — rule the reaper in or out (no debug kernel needed)

The static read of path.c raises a real question about whether "forever" is
literally forever. `tbv_path_expire_tx_ring_frames()` (path.c:1053-1105) is a
*time-based* reaper gated on `tx_queue_timeout_ms`, and it is the only thing that
decrements `tx_inflight` for a frame the ring took and never completed.

Confirmed on the hosts: `tx_queue_timeout_ms=30000` and `data_tx_max_inflight=32`,
both nonzero, so the reaper is configured. But it is only re-armed from within
`tbv_path_tx_poll_work()`, and only where `tx_poll_enabled` is true
(`tbv_path_progress_poll_enabled()`, path.c:385-392).

**What it proves.** Whether the permanent wedge is a genuinely lost hardware
completion, or a lost completion plus a reaper that never runs. These need different
fixes. The observed `data_tx_posted - data_tx_completed == 32 == data_tx_max_inflight`
says the ceiling was reached; if the reaper were running, that number should decay
after 30 s.

**Cost.** Free — re-run the reproducer and snapshot the counters at t+35 s and
t+70 s instead of only at the moment of the wedge.

**Risk.** None.

### P2 — static tracepoints in the ring code

There are currently **no** tracepoints on the ring path at all. `trace.h` in
thunderbolt_ibverbs defines only `tbv_cfgfs_link_op` and `tbv_active_link`, neither
ring-related. Upstream `drivers/thunderbolt/trace.h` has `tb_tx`/`tb_rx`/`tb_event`,
but those instrument the *control channel* in `ctl.c`, not the NHI DMA ring
(https://lore.kernel.org/linux-usb/20240209142609.2288471-1-mika.westerberg@linux.intel.com/).

Confirmed in this fork as well: `drivers/thunderbolt/trace.h` defines the `tb_raw`
event class plus `tb_tx`, `tb_event` and `tb_rx`, and the only file with
`CREATE_TRACE_POINTS` is `ctl.c`. `nhi.c` has no tracepoints at all.

So there is nothing to enable; instrumentation has to be added.

**Static `TRACE_EVENT` beats kprobes here, for reasons the docs state directly.**
`Documentation/trace/tracepoints.rst`: when a tracepoint is off it "has no effect,
except for adding a tiny time penalty (checking a condition for a branch)". The
`trace_<name>_enabled()` guard is backed by a jump-label static key, so argument
computation is skipped entirely with no runtime branch. And "verification of probe
type correctness is done at the registration site by the compiler" — typed,
compiler-checked arguments, versus a kprobe's `+4($stack)` register-offset guessing
that silently breaks whenever the compiler reorders a stack frame.

That last point is decisive for this bug specifically. `ring->head`, `ring->tail`
and `ring->descriptors[]` are private to nhi.c; path.c has no visibility into them at
all. A kprobe on `ring_write_descriptors` would have to reconstruct them from
register state in an inlined, optimised function. A tracepoint just passes them.

**What to add.** The single most valuable one is in `ring_write_descriptors()`
(nhi.c:253-276), after the descriptor is filled and again after the doorbell:

```
tb_ring_desc_post(hop, is_tx, head, tail, size, phys, length, sof, eof, flags)
```

plus `tb_ring_desc_complete(hop, is_tx, tail, flags)` on each `RING_DESC_COMPLETED`
observation in `ring_work()`/`tb_ring_poll()`, and `tb_ring_poll_empty(hop, is_tx,
head, tail)` on the not-completed branch so the stalled descriptor's exact head/tail
delta is visible without a debugger.

**What it proves.** Exactly which descriptor index stalled, what its flags and
length/sof/eof were at post time, and where head and tail sat relative to it. That
distinguishes "hardware never wrote COMPLETED" from "software never looked" from
"the descriptor was malformed when posted" — the last being the direct prediction of
the P0 hypothesis.

**Cost.** Near zero when disabled. Needs a module rebuild, not a kernel rebuild
(tracepoints can be defined in the out-of-tree module).

**Risk.** Low. Note that the existing diagnostics are `pr_warn_ratelimited`
(path.c:1099-1102, 1211-1221), which can be rate-limited away exactly when they
matter most — a tracepoint does not have that failure mode.

### P3 — `CONFIG_DMA_API_DEBUG`

**What it proves or rules out.** Whether any TX buffer is handed to the device
without the ownership transfer the DMA API requires. `Documentation/core-api/dma-api.rst`
states the rule: "DMA_TO_DEVICE synchronisation must be done after the last
modification of the memory region by the software and before it is handed off to the
device."

The copied path is already correct, and it is what the fleet actually uses (the
driver's own capture comments at path.c:3496-3498 note every capture shows
`data_wr_zcopy = 0` and `data_wr_copied > 0`). `tbv_path_schedule_tx()` does
`memcpy` (path.c:2529) then `dma_sync_single_for_device()` (path.c:2539-2541) then
`tb_ring_tx()` (path.c:2543). Correct ordering.

The genuine target is the **premapped zero-copy path** (path.c:3050-3067), which
hands a persistently-mapped MR address to `tb_ring_tx()` with no
`dma_sync_single_for_device()` anywhere. That is the one place in the TX pipeline
where "write to mapped buffer, no sync, hand to device" literally happens. It is
probably fine on cache-coherent x86, but "probably fine" is what this option exists
to convert into a fact.

**However — and this materially lowers the expected yield — the zero-copy path is
not exercised by this reproducer at all.** Every `data_wr_zcopy_*` counter in the
baseline capture is zero (`data_wr_zcopy_staged`, `_mr_mapped`, `_remapped`, and all
five fallback counters). So the one code path `DMA_API_DEBUG` was going to
scrutinise never runs during the failure. Run it to close the question out
permanently, and to catch any map/unmap bookkeeping error on the copied path, but do
not expect it to explain this stall. This is the clearest example of why P0 and P2
rank above it.

**Cost.** The doc is blunt: "Enabling this option has a performance impact. Do not
enable it in production kernels." It is core code, not compiler instrumentation, so
the cost is per-DMA-call bookkeeping rather than a global slowdown. Critically,
`dma_debug_driver=thunderbolt_ibverbs` scopes reporting to one driver, which keeps
the perturbation small enough that a timing-dependent bug still has a chance to
reproduce. Also set `dma_debug_entries=` well above the default 65536 given the frame
rate here.

**Kernel rebuild.** Yes.

**Risk to a live host.** Moderate. It cannot be re-enabled at runtime once disabled
("you can not enable it again at runtime. You have to reboot to do so"), and by
default only the first error is reported — set `/sys/kernel/debug/dma-api/all_errors`
to 1 or the second and subsequent problems are invisible.

**Honest note.** I could not find any published workflow connecting `DMA_API_DEBUG`
to stuck-descriptor debugging. It is the right tool for the sync-correctness
hypothesis and the wrong tool for the barrier hypothesis. Run it to close out the
zcopy question, not because it is likely to find the stall.

### P4 — `CONFIG_KCSAN`

**What it proves or rules out.** Genuine CPU-CPU data races in the driver. The
driver feeds and polls the ring from multiple contexts and KCSAN is built for exactly
that. There is at least one confirmed mixed-access field to go after:
`path->tx_ring_stalled` is written as a **plain store under `tx_lock`** (path.c:995),
written with **`WRITE_ONCE` outside any lock** (path.c:1098), and read with
**`READ_ONCE` under no lock** (path.c:204). Three call sites, three different
disciplines, same field.

There is also a structural risk KCSAN is well placed to probe: nhi.c's `ring->lock`
and path.c's `path->tx_lock` are entirely independent locks guarding two lists
(`ring->in_flight` versus `path->tx_frame_inflight`/`tx_zcopy_inflight`) that stay
consistent only because `frame->callback` is assumed to fire exactly once per frame.

`CONFIG_KCSAN_WEAK_MEMORY` is the reason this is worth the slowdown — it models
load/store buffering and flags missing `smp_mb()`/`smp_wmb()`/`smp_rmb()`/
`smp_store_release()`. It requires `CONFIG_KCSAN_STRICT=y`, which "configures KCSAN
to follow the Linux-kernel memory consistency model (LKMM) as closely as possible".

**Cost.** The docs give real numbers: "5.0x slow-down with the default KCSAN config;
2.8x slow-down from runtime fast-path overhead only". Memory overhead is only a few
MiB.

**Kernel rebuild.** Yes, and the instrumentation is a compiler flag, so the
out-of-tree DKMS modules must be rebuilt against the KCSAN kernel to be covered
(kbuild applies the sanitizer flags to external modules automatically).

**Risk to a live host.** This is the flavour most likely to trip the 120 s watchdog,
and a 5x slowdown may well make a ~1-in-450k timing race stop reproducing entirely.
Build it with `CONFIG_KCSAN_EARLY_ENABLE=n` and arm it via `/sys/kernel/debug/kcsan`
immediately before the run, so boot happens at full speed.

**Accuracy caveat, stated by the docs.** "due to using a sampling strategy, the
analysis is *unsound* (false negatives possible), but aims to be *complete* (no false
positives)". A clean KCSAN run does not prove there is no race. A KCSAN report is
almost certainly a real race.

### P5 — `CONFIG_KASAN`, and `KFENCE` as the cheap alternative

**What it proves or rules out.** Memory corruption — a use-after-free on a
`ring_frame` or a `tbv_data_frame` that leaves a descriptor pointing at freed memory.

**Priority is low, deliberately.** The evidence points away from corruption. The
failure is clean and structured: exactly `data_tx_max_inflight` descriptors
outstanding, counters self-consistent, no oops, no list corruption, and the host
survives indefinitely in the stalled state. Corruption bugs rarely present this
tidily.

**Cost.** Generic KASAN is roughly 3x slower with ~50% memory overhead for dynamic
allocations (outline), or ~20% with inline instrumentation. The docs frame generic
KASAN as debugging-only; the hardware-tag mode intended for production is arm64/MTE
and unavailable here.

**Do not combine KASAN with KCSAN.** 3x on top of 5x will almost certainly mask a
race this narrow, and the two are separately actionable anyway.

**`KFENCE` is the better default.** "KFENCE is designed to be enabled in production
kernels, and has near zero performance overhead." It is sampling-based, so it trades
precision for cost — it will not reliably catch a specific corruption, but it costs
nothing to leave on in every flavour. `CONFIG_KFENCE_NUM_OBJECTS=1023` widens the
pool at `(1023+1)*2*4KiB` = 8 MiB.

`CONFIG_DEBUG_LIST` is also worth having in every flavour: the ring's `in_flight`
list is exactly the structure whose corruption would produce a lost frame, and list
debugging is cheap.

### P6 — live inspection with kgdb/kdb

**Assessment: viable over IPMI SOL, and worth setting up, but not first.**

Both hosts expose `/dev/ttyS0`. Secure Boot is disabled and the kernel lockdown mode
is `[none]` on both, so kgdb is not blocked. IPMI Serial-over-LAN redirects the
existing UART through the BMC over the network, and the OS-visible device stays a
normal UART, so `kgdboc=ttyS0,115200` works against SOL exactly as against a physical
port (https://en.wikipedia.org/wiki/Serial_over_LAN). I did not verify SOL
specifically on the SP5100 BMC on these boards — that is the one thing to confirm
before relying on it.

**`kgdboe` is not an option.** It has never been merged to mainline; LWN treats it
as an out-of-tree add-on (https://lwn.net/Articles/375049/). The design problem is
that it depends on netpoll, which is not safe under IRQ preemption because the
ethernet device cannot be reliably shared between polled and interrupt-driven
contexts. Do not plan around it.

**What it would let you inspect.** Exactly the state that is otherwise invisible:
`ring->head`, `ring->tail`, the full `ring->descriptors[]` array at the stalled index
with its `flags`/`length`/`sof`/`eof`, the `ring->in_flight` list, and the NHI
registers via the mapped BAR — at the moment of the stall, with the machine frozen so
nothing races you while you look. `kdb` alone (no gdb) gets you memory/register dump,
`bt`, `ps`, `lsmod`, `dmesg` — the docs are clear that "kdb is not a source level
debugger", so use full gdb over the serial link for structure inspection.

**Config requirements.** `CONFIG_KGDB`, `CONFIG_KGDB_SERIAL_CONSOLE`,
`CONFIG_KGDB_KDB`, plus `CONFIG_DEBUG_INFO` for symbols and `CONFIG_FRAME_POINTER`
for accurate backtraces. `CONFIG_STRICT_KERNEL_RWX` must be **off** or software
breakpoints do not work. `nokaslr` is needed because KASLR "confuses gdb which
resolves addresses of kernel symbols from the symbol table of vmlinux" — the build
below sets `CONFIG_RANDOMIZE_BASE=n` rather than relying on a cmdline argument.

**Risk to a live host.** High if used carelessly. Entering the debugger freezes every
CPU; the 120 s systemd watchdog will reset the box while you sit at a prompt. Disable
`RuntimeWatchdogSec` before any kgdb session. This is why kgdb is P6 and not P2 —
the same state is obtainable more safely from tracepoints or a crash dump.

### P7 — post-mortem with kdump/crash

**`crashkernel=` is already on the fleet cmdline**, confirmed on both hosts:
`crashkernel=2G-4G:320M,4G-32G:512M,32G-64G:1024M,64G-128G:2048M,128G-:4096M`. At
60 GB RAM these hosts land in the `32G-64G:1024M` bucket. So the reservation exists
and only `kdump-tools` plus `CONFIG_CRASH_DUMP` need to be in place.

**What a forced dump gives you that live tracing cannot.** A tracepoint stream tells
you what the driver *did*; a dump tells you what memory *is*. At the stall you can
walk the entire `struct tb_ring` — every one of the 1024 descriptors, not just the
ones something chose to trace — and compare `ring->head`, `ring->tail`, the hardware's
own head/tail as last read from the NHI BAR, and the `in_flight` list, all as a single
consistent snapshot. Critically it also lets you inspect the *other* 31 stalled
descriptors, which no ad-hoc trace was set up to capture, and answer whether they are
contiguous (one hardware stop) or scattered (repeated independent losses). Those two
answers point at completely different root causes.

Trigger it deliberately at the wedge with `echo c > /proc/sysrq-trigger`, then
`makedumpfile -l --message-level 1 -d 31 /proc/vmcore <file>` and analyse with
`crash`. Note the docs warn that GDB "cannot analyze core files generated in ELF64
format for x86", so use the crash utility rather than plain gdb.

**Cost.** Zero until you trigger it. The dump itself is minutes.

**Risk.** Taking the dump is a hard crash of the node — go through
`graceful-drain.sh` first, exactly as for a reboot.

### P8 — hardware error paths

This has to be ruled in or out explicitly, because if the hardware is dropping a
descriptor for a link-level reason then no amount of software tracing will ever find
it. A prior capture showed a non-trivial rate of `data_rx_crc_error_standalone`, so
link quality is a live hypothesis.

**What was checked, and the result.**

*Host-side PCIe AER is clean.* Both `aer_dev_correctable` and `aer_dev_nonfatal` are
all-zero across every PCI device on appmana-023, and `dmesg` shows AER enabled on the
root ports with no errors logged. There is no PCIe-level problem between the CPU and
the NHI.

*IOMMU is clean.* Zero `IO_PAGE_FAULT` or `AMD-Vi` event entries in dmesg. This
matters because an IOMMU translation failure is the standard way for a legitimate DMA
to be silently dropped, and it is not happening. I found no evidence in the
literature that AMD-Vi causes NHI ring stalls; the upstream Thunderbolt/IOMMU work is
about DMA *protection* against rogue devices, not reliability
(https://patchwork.ozlabs.org/project/linux-pci/cover/20181112160628.86620-1-mika.westerberg@linux.intel.com/).

*Thunderbolt port counters exist but are raw.*
`/sys/kernel/debug/thunderbolt/<domain>/<port>/counters` gives
`offset relative_offset counter_id value` triples with no symbolic names — decoding
requires Intel's `tbtools` (https://github.com/intel/tbtools), which is the
maintained reference for these counters and also provides `tbmargin` for lane
margining. Lane margining is implemented in-kernel as `usb4_port_sw_margin()` /
`usb4_port_sw_margin_errors()` behind `CONFIG_USB4_DEBUGFS_MARGINING`.

**How to settle the CRC question.** Two steps, in order:

1. Snapshot every port's `counters` before and after a reproducer run and diff them.
   If the error counter deltas are flat across a run that stalls, link quality is
   not the trigger for *this* event even if the standing rate is nonzero.
2. If they are not flat, run lane margining with `tbmargin` to get an eye-margin
   number, and re-run on a different cable and a different adjacent pair to see
   whether the rate follows the cable or the host.

**Why a nonzero CRC rate is probably not the cause anyway.** The USB4 logical layer
provides its own error detection, forward error correction and recovery
(https://www.usb.org/sites/default/files/D1T1-5%20-%20USB4%20Logical%20Layer%20-%20Retimer%20-%20Transport.pdf).
Link-level errors are expected to be retransmitted transparently below the tunnelling
layer, so a CRC error should cost latency, not a lost DMA descriptor. I could not find
a kernel source comment or spec line stating this outright for the tunnelled-DMA case,
so treat it as inference from the spec rather than an established fact — but it means
CRC errors are better read as a *cable health signal* than as a direct explanation.

**Also worth ruling out:** the upstream interrupt-masking bug class. The
`tb_ring_poll_complete()` comment in this fork (nhi.c:441-453) documents an
already-fixed race where the ring interrupt could stay masked forever — "one bit in
REG_RING_INTERRUPT_BASE stays cleared and MSI-X stops". TX here always allocates with
`start_poll = NULL` (path.c:1787-1789) so it is interrupt-driven via `ring_work()`
and that specific fixed race does not apply, but reading
`REG_RING_INTERRUPT_BASE` at the stall is a cheap direct check that the TX ring's
interrupt bit is still set.

### P9 — things not otherwise listed that are standard and cheap

- **`CONFIG_DEBUG_LIST`** — the `ring->in_flight` and `path->tx_frame_inflight` lists
  are exactly what a lost frame would corrupt. Cheap. Enabled in all flavours below.
- **`CONFIG_DEBUG_OBJECTS`** and **`CONFIG_DEBUG_OBJECTS_WORK`** — catches a
  `work_struct` (here `ring->work`) being re-initialised or freed while queued, which
  would silently drop the completion handler. Directly relevant to a "completion never
  ran" symptom.
- **lockdep (`CONFIG_PROVE_LOCKING`)** — deliberately left out of the flavours below.
  The repo already has `.lockdep-run/` and `docs/lock_deadlock_verification.md`, so
  this ground is covered, and lockdep's overhead is another timing perturbation for no
  new information.
- **`CONFIG_DEBUG_KMEMLEAK`** — not useful here. Frames are pooled, not leaked, and
  the doc warns it does not track page allocations at all.
- **`CONFIG_UBSAN`** — cheap, and the bitfield arithmetic in `struct ring_desc` is
  exactly the kind of code where a shift/truncation bug hides. Low priority but not
  zero.
- **Sentinel descriptors** — from the Keeter writeup: deliberately write a recognisable
  poison value into the descriptor's unused `time` field before posting, and check it
  at completion. If the device ever acts on a descriptor whose sentinel is stale, that
  proves it read the descriptor before the driver finished writing it. This is a
  direct, cheap test of the P0 hypothesis and needs no debug kernel.
- **`nhi_interrupt_throttle_ns`** — already a module parameter (currently 0). Sweeping
  it is a zero-cost way to move the timing and confirm the failure is a race rather
  than a deterministic state bug.

## summary table

| # | technique | proves / rules out | runtime cost | kernel rebuild | risk to live host |
|---|---|---|---|---|---|
| P0a | `WARN_ONCE` on NULL-callback frame | completion silently dropped by nhi.c | none | no | none |
| P0b | `dma_wmb()` + field reorder experiment | CPU/device store ordering | none | no | none |
| P1 | reaper (`tx_queue_timeout_ms`) audit | lost completion vs lost reaper | none | no | none |
| P2 | static `TRACE_EVENT` in ring code | which descriptor, what flags, head/tail | ~0 when off | no | low |
| P3 | `CONFIG_DMA_API_DEBUG` | missing/incorrect sync, double-map, use-after-unmap | moderate, scopeable | yes | moderate |
| P4 | `CONFIG_KCSAN` (+`WEAK_MEMORY`) | CPU-CPU races, missing smp barriers | ~5x | yes | high, may mask bug |
| P5 | `CONFIG_KASAN` / `KFENCE` | memory corruption | ~3x / ~0 | yes | high / none |
| P6 | kgdb/kdb over IPMI SOL | live ring + descriptor state | none until entered | yes | high, freezes CPUs |
| P7 | kdump + crash | full consistent snapshot of all 1024 descriptors | none until triggered | yes | crashes node |
| P8 | AER, TB counters, tbmargin | link-level drop | none | no | none |

## what the debug kernel is actually for

Given the two leading hypotheses are a frame lifecycle race (P0a) and a store
ordering bug (P0b), and no sanitizer can see either directly, the debug kernel's job
is narrower than it first appears. It exists to:

1. Find the path.c races that KCSAN *can* see (P4). This is now the debug kernel's
   most valuable job, because if the completion is being lost to a frame recycled
   through `path->tx_free` while the ring still holds it, that is a genuine CPU-CPU
   race between two lock domains and it is exactly what KCSAN was built for. Start
   with `path->tx_ring_stalled` (path.c:204, 995, 1098), then watch `f->frame.callback`
   and the `free_node` list transitions.
2. Close out the zero-copy sync question with `DMA_API_DEBUG` (P3) — low expected
   yield, since the reproducer never touches that path.
3. Provide the symbols, `kgdb` and `kdump` plumbing needed for P6 and P7 when the
   cheaper techniques run out. A dump is the only way to see all 1024 descriptors and
   both inflight lists as one consistent snapshot, which is what settles whether the
   32-frame and 1-frame signatures are the same bug.

It is not expected to print a report that names the bug. Expect it to eliminate
hypotheses, and expect P0a and P2 to be what actually identifies the mechanism.

## what the debug kernel actually showed

Run on appmana-025 and appmana-023, both on `6.17.13-tbdma`, with
`dma_debug_entries=262144` and `/sys/kernel/debug/dma-api/all_errors` set to 1.
Reproducer: `TBV_QPS=32 TBV_SIZE=262144 tbv-hang-repro.sh appmana-025 appmana-023`.

**The bug still reproduces under the debug kernel, in 2 s** — faster than the 13 s
it took on the stock kernel. That is the first useful result: `DMA_API_DEBUG` and
`KFENCE` do not perturb the timing enough to hide it, so this flavour is a valid
platform for the rest of the work.

### DMA API — clean, hypothesis eliminated

```
=== dma-api error_count ===
0
=== dma-api min_free_entries / nr_total ===
248514
262144
[    0.316975] DMA-API: preallocated 262144 debug entries
[    0.316978] DMA-API: debugging enabled by kernel config
```

Zero errors on both hosts, with `all_errors=1` so it was not merely reporting the
first one. 13,630 entries were in use at peak on appmana-025, so the pool was
nowhere near exhaustion and nothing was silently dropped.

**This eliminates the "buffer never synced to the device" hypothesis.** No missing
`dma_sync_single_for_device`, no wrong-direction sync, no double map, no
use-after-unmap, on either the copied or the control path.

### KFENCE, DEBUG_LIST, KASAN-class — clean

```
[    0.136065] kfence: initialized - using 8388608 bytes for 1023 objects
```

No KFENCE reports, no `list_add`/`list_del` corruption warnings, no `BUG:`, no
`WARNING:`, no call traces on either host. Nothing supports the memory corruption
hypothesis.

### PCIe AER — clean

```
[    0.499277] pcieport 0000:00:01.1: AER: enabled with IRQ 27
[    0.499452] pcieport 0000:00:02.1: AER: enabled with IRQ 28
```

Zero correctable, nonfatal and fatal counts across every PCI device on both hosts.
No PCIe-level problem between the CPU and the NHI.

### the NHI registers settle the open question

The driver's head/tail are software shadows. The hardware's own view lives only in
the NHI BAR0 registers, so `tools/nhi-ring-regs.py` was written to read them
directly. At the wedge:

**appmana-025, hop 3 — the stuck TX ring**

```
TX desc_phys=0x00000002868dc000 count_reg=0x00000400
TX prod/cons raw=0x027b0000 driver_head=635 nhi_tail=0 outstanding=635
TX options=0x80000000 [ENABLE]
```

The ring is enabled, the descriptor base and the 1024-entry count are correctly
programmed, the driver has published head=635 — and **the NHI consumer index is
still 0. The hardware has not consumed a single descriptor.**

**appmana-023, hop 3 — the far end of that same link**

```
RX prod/cons raw=0x000003ff driver_head=1023 nhi_tail=0 outstanding=1023
RX options=0x80000000 [ENABLE]
TX prod/cons raw=0x00410041 driver_head=65 nhi_tail=65 outstanding=0
```

The receiver has **1023 free RX descriptors posted and armed**, and has received
nothing. Its own TX ring is perfectly healthy — `driver_head == nhi_tail == 65`,
nothing outstanding.

**Conclusion: the TX engine is stopped locally on appmana-025. It is not
backpressured by the wire.** The receiver has an entirely empty, fully-armed RX
ring, so there is nothing for it to backpressure with. And `RING_FLAG_E2E_FLOW_CONTROL`
is clear on every ring on both hosts (`options=0x80000000` is `ENABLE` alone), so
no hardware end-to-end flow control is even active. This also means enabling
`RING_FLAG_E2E` would not address this failure.

The driver-level counters agree:

```
appmana-025  data_tx_enqueued=65520 data_tx_posted=32 data_tx_completed=0
             control_tx_enqueued=543 control_tx_posted=543 control_tx_completed=0
             tx_poll enabled=1 calls=1333 completed=0
             data_rx_completed=9598 data_rx_credit_sent=9536
appmana-023  data_tx_posted=17473 data_tx_completed=17473  tx_credits=0/768
             data_tx_credit_stalls=2709 data_tx_credit_received=0
             data_rx_completed=0
```

Note `control_tx_posted=543 control_tx_completed=0`. **The ring stopped completing
control frames too, not just data.** The whole hop is dead, not one traffic class.
And 1333 polls read `ring->descriptors[tail].flags` directly and never saw
`RING_DESC_COMPLETED`.

The rest is a self-reinforcing deadlock, not independent bugs: appmana-025 is
correctly generating credits for appmana-023 (`data_rx_credit_sent=9536`), but
those credits have to go out over the dead TX ring, so appmana-023 sees
`data_tx_credit_received=0`, starves at `tx_credits=0/768`, and stops. Every other
symptom is downstream of the one stuck TX ring.

### the detail that does not fit, stated honestly

`nhi_tail=0` is not "advanced then stopped". It is **never advanced at all**. Taken
with `control_tx_completed=0` after 543 control frames, this instance looks like a
TX hop that never consumed anything from the moment the ring was started — a setup
or fabric-programming problem — rather than a ring that ran for ~450k frames and
then lost one descriptor.

That is a different shape from the archived `posted=451771 completed=451770`
signature. Either there are two distinct failures, or the 451k case is this same
failure caught after a path re-establish reset the registers. Deciding that is the
next question, and the cheap way to settle it is to sample
`tools/nhi-ring-regs.py` on a loop during a run and watch whether `nhi_tail` ever
moves before it wedges.

### what this means for the tool priorities above

- P3 (`DMA_API_DEBUG`) is **done and negative**. Do not spend more time on it.
- P5 (KASAN) is now very hard to justify — KFENCE and DEBUG_LIST were clean and the
  failure is in the hardware's consumer index, not in memory contents.
- P8 (hardware) is partially settled: PCIe AER is clean, but the TB link CRC
  counters still need the before/after diff, and the NHI TX engine refusing to
  consume an enabled, correctly-programmed ring is itself a hardware-adjacent
  finding.
- P0b (the missing `dma_wmb()`) is **not** disproven and remains worth fixing on its
  own merits, but it cannot explain `nhi_tail=0` — a barrier bug would corrupt a
  descriptor, not stop the engine from consuming any descriptor ever.
- P4 (KCSAN) remains the best use of a further debug kernel, for the frame lifecycle
  question.

## references

Kernel documentation, read from the v6.17 tree:

- `Documentation/core-api/dma-api.rst` — ownership rules, `DMA_API_DEBUG`,
  `dma_debug_driver=`, `dma_debug_entries=`, the `dma-api/` debugfs interface
- `Documentation/core-api/dma-api-howto.rst` — coherent vs streaming mappings
- `Documentation/core-api/wrappers/memory-barriers.rst` — `dma_wmb()` before the
  ownership bit, then `writel()` for the doorbell
- `Documentation/dev-tools/kcsan.rst` — data race definition, 5.0x/2.8x slowdown
  figures, `KCSAN_STRICT`, `KCSAN_WEAK_MEMORY`, unsound-but-complete property
- `Documentation/dev-tools/kasan.rst` and `lib/Kconfig.kasan` — modes, ~3x slowdown,
  ~50% (outline) / ~20% (inline) allocation overhead
- `Documentation/dev-tools/kfence.rst` — near-zero overhead, pool sizing formula
- `Documentation/process/debugging/kgdb.rst` — `kgdboc=` syntax, `STRICT_KERNEL_RWX`
  and `nokaslr` requirements, kdb scope
- `Documentation/admin-guide/kdump/kdump.rst` — `crashkernel=` syntax, sysrq-c,
  makedumpfile
- `Documentation/PCI/pcieaer-howto.rst` — error classes, ratelimiting, `_OSC`
- `Documentation/trace/tracepoints.rst` and `events.rst` — cost when off, static-key
  guards, compiler-checked probe signatures

External:

- Matt Keeter, "Hunting a spooky ethernet driver bug" — https://www.mattkeeter.com/blog/2023-10-31-dma/
- `mmc: cqhci: commit descriptors before setting the doorbell` — https://lkml.iu.edu/hypermail/linux/kernel/1910.1/06233.html
- `scsi: ufs: commit descriptors before setting the doorbell` — https://lkml.iu.edu/hypermail/linux/kernel/1508.3/03124.html
- `dma: fsl-edma: Add write barrier after TCD descriptor fill` — https://lkml.iu.edu/2511.3/03097.html
- `thunderbolt: Mask ring interrupt properly when polling starts` — https://lkml.iu.edu/hypermail/linux/kernel/1712.0/00296.html
- `ring_interrupt_active()` regression — https://bugzilla.redhat.com/show_bug.cgi?id=1703369
- thunderbolt control-channel tracepoints — https://lore.kernel.org/linux-usb/20240209142609.2288471-1-mika.westerberg@linux.intel.com/
- Intel tbtools — https://github.com/intel/tbtools
- USB4 logical layer error recovery — https://www.usb.org/sites/default/files/D1T1-5%20-%20USB4%20Logical%20Layer%20-%20Retimer%20-%20Transport.pdf
- KCSAN in LWN — https://lwn.net/Articles/800298/ and https://lwn.net/Articles/816850/
- kgdboe is out-of-tree — https://lwn.net/Articles/375049/
- Thunderbolt IOMMU DMA protection — https://patchwork.ozlabs.org/project/linux-pci/cover/20181112160628.86620-1-mika.westerberg@linux.intel.com/
