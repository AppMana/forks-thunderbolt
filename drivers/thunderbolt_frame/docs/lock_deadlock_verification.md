# Lock deadlock verification

What has and has not been machine-checked about lock-related deadlock in the
`thunderbolt_ibverbs` driver, and how to re-run each check.

The trigger was a real self-deadlock found by reading: `tbv_cq_push()` on CQ
overflow called `tbv_qp_mark_error()` -> `tbv_qp_flush_error()` ->
`mutex_lock(&tqp->rx_lock)` from four callers that already held `rx_lock`.
It is fixed, but the KUnit that pins it only asserts a structural property of
that one call site. This document covers the general question.

## Tools

| Check | Tool | Command |
|---|---|---|
| runtime lock validation over the KUnit suite | kernel lockdep | `KUNIT_LOCKDEP=1 tools/run-kunit.sh` |
| lock context balance | sparse | `make C=2 CHECK=sparse drivers/thunderbolt_ibverbs/` in the overlay tree |
| rx_lock re-entry pattern | Coccinelle | `spatch -D report --sp-file tools/rx_lock_reentry.cocci --dir kernel` |
| whole lock graph and transitive closure | `tools/lock-graph.py` | `tools/lock-graph.py` |

## Derived lock graph

Fifteen lock instances appear in the module. The ones that participate in any
nesting, with the orders actually present in the source:

```
qp.rx_lock       (mutex)   -> qp.lock (spinlock, irqsave)
                            -> cq.lock (spinlock, irqsave)
                            -> path.tx_lock (spinlock, irqsave)
                            -> state.lock (mutex)
read_ctx.data_lock (mutex) -> read_ctx.lock, qp.lock, cq.lock,
                              path.tx_lock, state.lock
peer.control_lock (mutex)  -> state.lock
                            -> state.rail_register_lock -> state.lock
                            -> path.tx_lock
state.rail_register_lock   -> state.lock
state.lock (mutex)         -> qp.lock
                            -> path.tx_lock
link.lock (mutex)          -> state.lock
link_owner.lock (mutex)    -> path.tx_lock
```

Two structural facts fall out and both are good:

* No spinlock is ever the outer lock of a nesting. `qp.lock`, `cq.lock` and
  `path.tx_lock` only ever appear innermost, so no sleeping lock is taken in
  atomic context by a named call.
* The mutex order is a strict hierarchy — `link.lock` / `peer.control_lock` /
  `qp.rx_lock` / `read_ctx.data_lock` above `state.rail_register_lock` above
  `state.lock` — with exactly one exception, below.

## Result 1: lockdep is clean over the whole suite

`KUNIT_LOCKDEP=1 tools/run-kunit.sh` builds a `PROVE_LOCKING` /
`DEBUG_SPINLOCK` / `DEBUG_MUTEXES` / `DEBUG_ATOMIC_SLEEP` / `DEBUG_LOCKDEP` /
`PROVE_RCU` kernel into a separate build dir and runs every suite. All tests
pass with zero splats: no recursive acquisition, no circular dependency, no
irq-safe/irq-unsafe inversion, no sleeping in atomic.

The harness was validated with a positive control: a throwaway KUnit taking two
mutexes A-then-B and then B-then-A produced
`WARNING: possible circular locking dependency detected` in the run output, so
a splat in driver code would have been visible. The control was removed after
it served its purpose.

### What lockdep did not see

lockdep only validates orderings the tests exercise. `tools/lock-graph.py
--coverage` computes which locks any KUnit entry point can reach:

reachable: `qp.rx_lock`, `qp.lock`, `cq.lock`, `path.tx_lock`, `state.lock`,
`send_ctx.lock`, `read_ctx.lock`, `link_owner.lock`

**not reachable**: `peer.control_lock`, `state.rail_register_lock`,
`link.lock`, `session.lock`, `identity.lock`, `read_ctx.data_lock`,
`tbv_lifecycle_lock`

So every native control-plane ordering — the whole HELLO/READY/tunnel path in
`native_control.c`, and the configfs and tbnet-identity locks — is
lockdep-unverified. Reaching it from a KUnit needs a `tb_xdomain` and a live
XDomain transaction, which the suite does not fake.

## Result 2: no remaining rx_lock re-entry

Two independent checks agree.

Coccinelle, `tools/rx_lock_reentry.cocci`:

```
@acq@ identifier f; expression q; @@
f(...) { ... mutex_lock(&q->rx_lock) ... }

@reent depends on report@ identifier caller, acq.f; expression q; @@
caller(...) {
  ...
  mutex_lock(&q->rx_lock)
  ... when != mutex_unlock(&q->rx_lock)
  f(...)
  ...
}
```

Zero hits over `kernel/`. The patch was validated on a synthetic file with the
historical shape (a holder calling a direct acquirer) and reports it, and does
not report the same call made after the unlock.

`tools/lock-graph.py`, which computes the unbounded transitive closure the
semantic patch cannot, also reports no path that reaches `mutex_lock(rx_lock)`
from a holder. It is validated against the pre-fix tree
(`91c1de8^`), where it reproduces the historical deadlock as ten distinct
caller paths.

`rx_lock` has seven direct acquirers, none of them nested:
`tbv_ibdev_rx_apple_frame`, `tbv_post_recv`, `tbv_qp_flush_error`,
`tbv_qp_timeout_reap_rx`, `tbv_rx_handle_rdma_read_req`,
`tbv_rx_handle_rdma_write_fragment`, `tbv_rx_handle_send_fragment`.

Naming is not evidence and was not used as such: `_locked` suffixes in this
driver name three different locks. `tbv_qp_drain_ready_sends_locked` means
`tqp->lock`, `tbv_rx_finish_write_locked` means `rx_lock`, and
`tbv_qp_ack_history_store_locked` is named like a `tqp->lock` helper but runs
under `rx_lock`.

## Result 3: sparse finds no lock context imbalance

`make C=2 CHECK=sparse` over the module (including `ibdev.c`, `path.c`,
`native_control.c`) produces no `context imbalance` or
`different lock contexts for basic block` warnings. Every acquisition is
released on every path sparse can see. The only sparse output is one unrelated
`symbol 'roce_netdev' was not declared` note.

## Defect found: `state->lock` self-deadlock in the identity refresh work

`tbv_native_control_identity_refresh_workfn()` (`native_control.c`) does:

```
mutex_lock(&state->lock);
list_for_each_entry(peer, &state->peers, node)
    list_for_each_entry(rail, &peer->rails, node)
        tbv_native_control_exchange_once(state, peer, rail, 1);
mutex_unlock(&state->lock);
```

and `tbv_native_control_exchange_once()` -> `tbv_native_control_apply_ack()` ->
`tbv_native_control_apply_remote()` opens with `mutex_lock(&state->lock)`.
Linux mutexes are not recursive, so the work item blocks forever holding the
device-wide `state->lock`, which wedges every other user of it (peer add and
remove, QP creation, rail registration).

It is the only remaining recursive acquisition that both models of
`lock-graph.py` agree on, and it is the sole cause of the one lock-order
inversion in the module: every other site takes `peer->control_lock` outside
`state->lock`, this one takes `state->lock` outside `control_lock`.

Reachability: the work is scheduled from the inetaddr notifier when the pinned
`roce_netdev` gets its IPv4 after the first HELLO already went out incomplete
(`hello_sent_incomplete`), i.e. the boot-vs-DHCP race the surrounding comments
describe. The deadlock is on the **success** path — a HELLO that times out or
fails to parse returns before `apply_ack()` and does not deadlock — which is
why it survives normal bring-up. Secondary problem at the same site:
`tb_xdomain_request()` sleeps for up to `TBV_NATIVE_HELLO_TIMEOUT_MS` per rail
under the global `state->lock`.

Not fixed here; this work was verification only. `lockdep_assert_not_held()`
annotations are in place at both `tbv_native_control_apply_remote()` and
`tbv_native_control_exchange_once()` so a `PROVE_LOCKING` kernel names the
offender instead of hanging.

## What remains unproven

* Every ordering involving `peer.control_lock`, `state.rail_register_lock`,
  `link.lock`, `session.lock`, `identity.lock`, `read_ctx.data_lock` and
  `tbv_lifecycle_lock` is unverified at runtime — no KUnit reaches them.
  The `state->lock` defect above lives entirely in that unreached region.
* `lock-graph.py` resolves direct named calls only. Locks taken behind function
  pointers (`ib_device_ops`, workqueue callbacks, XDomain protocol handlers,
  notifier chains) are outside the closure.
* Neither release model in `lock-graph.py` is sound. The linear model drops a
  lock at any unlock and so under-reports nesting after an error-branch unlock;
  the scoped model keeps a lock until an unlock at its own brace depth and so
  over-reports when an if/else unlocks in both arms. Findings are labelled
  `BOTH MODELS` or `scoped only`; only the former should be read as a claim.
* Locks in code the driver calls but does not own (thunderbolt core, the RDMA
  core, the workqueue and timer subsystems) are only covered to the extent the
  KUnit suite drives them.
* Deadlocks that are not lock-ordering deadlocks — unbounded waits on
  completions, credits or queue space — are out of scope here.

The defensible claim is: **lockdep is clean across the exercised paths, sparse
finds no context imbalance, and both a Coccinelle sweep and a transitive
closure over the call graph show no remaining call site that reaches
`mutex_lock(rx_lock)` from a holder.** Not: that no deadlock exists.
