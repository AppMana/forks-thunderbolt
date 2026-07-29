// SPDX-License-Identifier: GPL-2.0
//
// Find any path that re-enters mutex_lock(&...->rx_lock) from a caller that
// already holds rx_lock. This is the shape of the fixed cq_push self-deadlock:
// tbv_cq_push() -> tbv_qp_mark_error() -> tbv_qp_flush_error() ->
// mutex_lock(&tqp->rx_lock), reached from four callers already holding it.
// Linux mutexes are not recursive so the RX worker would block forever.
//
// Run:
//   spatch --very-quiet --no-includes --include-headers --timeout 240 \
//          -D report --sp-file tools/rx_lock_reentry.cocci --dir kernel
//
// acq   : every function that directly takes rx_lock.
// reent : a function that takes rx_lock and, before releasing it, calls one of
//         those. Any hit is a self deadlock.
//
// Coccinelle has no fixed-point operator, so this is a depth-1 check: it
// catches "holder calls a direct acquirer", which is the shape the bug takes
// once the intermediate helpers are inlined mentally, but it does not chain
// through arbitrary call depth. Chained inherited-identifier rules were tried
// and do not enumerate the whole callee set reliably. Use
// tools/lock-graph.py for the unbounded transitive closure; it reproduces the
// historical cq_push deadlock on the pre-fix tree in both of its models.

virtual report

@acq@
identifier f;
expression q;
@@
f(...) { ... mutex_lock(&q->rx_lock) ... }

@reent depends on report@
identifier caller, acq.f;
expression q;
@@
caller(...) {
  ...
  mutex_lock(&q->rx_lock)
  ... when != mutex_unlock(&q->rx_lock)
  f(...)
  ...
}

@script:python depends on report@
caller << reent.caller;
f << acq.f;
@@
if caller != f:
    print("REENTRY %s holds rx_lock and calls %s which takes rx_lock" % (caller, f))
else:
    print("REENTRY %s takes rx_lock twice" % caller)
