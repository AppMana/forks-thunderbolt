# Thunderbolt XDomain connection negotiation — shared model and the soft-reconnect fix

This documents the connection-negotiation logic shared by the Thunderbolt
host-to-host stacks we run, the bug they share, and the single common header
that fixes it. (Written against the legacy `thunderbolt_ibverbs` RDMA driver,
since removed; its successor `thunderbolt_frame` consumes the same shared
contract, and the legacy driver's behavior is kept below as the historical
record of the bug.)

- `thunderbolt` — the core driver (XDomain property exchange).
- `thunderbolt_net` — IP-over-Thunderbolt (`tbnet`).
- `thunderbolt_frame` — the frame service under the out-of-tree RDMA engine
  (registers the `"tbframe"` service; the legacy `thunderbolt_ibverbs`
  registered `"tbverbs"`).

## The shared negotiation model

All three negotiate a host-to-host connection in the same two layers, over the
same XDomain control channel (ring 0, `TB_CFG_PKG_XDOMAIN_REQ/RESP`):

1. **Property/identity exchange (owned by the core).** Each host advertises an
   XDomain *property directory* via `tb_register_property_dir()`
   (`thunderbolt_net` registers `"network"`, `thunderbolt_frame` registers
   `"tbframe"`; the legacy driver registered `"tbverbs"`). The remote block carries a **monotonic generation**. A peer
   re-reads the remote directory and accepts it only if the generation is
   strictly newer (`tb_xdomain_get_properties()` in `xdomain.c`). Service
   discovery (`enumerate_services()`) then binds the matching service driver
   and fires its `.probe`.

2. **A login/HELLO handshake (owned by each service driver).** On top of the
   discovered service, each driver runs a one-shot two-phase handshake:
   - `thunderbolt_net`: `login_sent` / `login_received` / `login_retries`
     (`start_login()`, `tbnet_login_work()`, `tbnet_login_request/response()`).
   - the legacy `thunderbolt_ibverbs`: `native_ready_sent` /
     `native_remote_ready` / `native_negotiated` / `native_ready_attempts`
     (`native_control.c`, removed with the legacy driver).
   Both ride `tb_xdomain_request()` / `tb_xdomain_response()`.

## The shared bug: soft reconnect never re-establishes

A **soft reconnect** — a service-driver reload, or a peer re-registering its
property directory — happens **without a physical link edge**. Two facts
combine to strand the peer:

1. **The ICM firmware only re-announces on a physical link edge.** Disassembly
   of both controller families (Titan Ridge JHL7540 and Maple Ridge JHL8540;
   Synopsys ARC firmware, see `../../icm-firmware-re/`) shows
   `ICM_EVENT_XDOMAIN_CONNECTED` is emitted exactly once per rising link-present
   edge, latched by a one-shot "announced" flag, with **no property-generation
   comparison and no re-notify path**. Maple Ridge additionally re-arms its
   announce on an inbound XDP properties-request/changed frame, but neither
   firmware compares a stored remote generation. So the ICM never tells the host
   a connected peer changed its properties.

2. **The kernel's only software re-notify is best-effort, and the generation
   gate silently drops re-reads.** The XDP `PROPERTIES_CHANGED` notification is
   retry-limited and racy. Worse, `tb_xdomain_get_properties()` drops any
   re-read whose generation is `<= remote_property_block_gen` (the cached value)
   — silently (`ret = 0`, no log). On a *simultaneous* fleet reload the
   notification is lost/raced and the receiver latches a stale generation, after
   which every later re-read is dropped, `enumerate_services()` never re-runs,
   `.probe` never re-fires, and the driver's one-shot handshake never re-arms.

This is why a coordinated `thunderbolt_ibverbs` reload across the chain leaves
peers un-negotiated, and why `tb-chain-reboot-cover.sh` resorts to **rebooting**
(a fresh boot is the only guaranteed physical link edge) to recover. It is the
same failure `thunderbolt_net` hits ("module reloads race the neighbour's
one-shot property re-read and usually lose").

## The fix: one common header

`drivers/thunderbolt/thunderbolt_negotiation.h` carries the shared primitives.
It is the single canonical copy: `thunderbolt_frame` includes it directly
(`frame/tbframe_priv.h`), and the packaging bundles this one header into the
DKMS source tree (the legacy driver's byte-identical `proto/` copy is gone).

- **`tb_xdomain_generation_stale(have_remote, remote_gen, cached_gen)`** — the
  generation gate, with one contract change: a `cached_gen` of **0** forces the
  next real block (generation >= 1) to be accepted. Callers set the cached
  generation to 0 on any reconnect signal, so a stale latch can never strand a
  peer. Used at the gate in `tb_xdomain_get_properties()`.

- **`struct tb_xdomain_handshake` + `tb_xdomain_handshake_reset()`** — the
  common login/HELLO state and the re-arm contract. `*_reset()` MUST be called
  on every soft reconnect (properties-changed notification, local service
  re-registration, and the core `rescan` trigger) so the handshake re-runs
  instead of staying latched.

### Core-side wiring (landed)

- `tb_xdomain_get_properties()` uses `tb_xdomain_generation_stale()`.
- On `PROPERTIES_CHANGED_REQUEST`, `remote_property_block_gen` is reset to 0 so
  a delivered notification always forces a fresh accept.
- A write-only per-xdomain **`rescan`** sysfs attribute resets the cached
  generation, tears stale `tb_service` children, and re-reads/re-probes — a
  firmware/CM-independent recovery that needs no cable replug:
  `echo 1 > /sys/bus/thunderbolt/devices/<xd>/rescan`.

### Driver-side adoption

- `thunderbolt_net`: replace the ad-hoc `login_sent`/`login_received` booleans
  with `struct tb_xdomain_handshake` and call `tb_xdomain_handshake_reset()` from
  its properties-changed path so a soft reconnect re-logs-in.
- `thunderbolt_frame`: the generation gate, re-announce and supersede
  semantics come from the shared contract via `frame/core.c`; the legacy
  driver's per-rail `native_*` booleans left with it.

## Validation

- Pure predicate `tb_xdomain_generation_stale()` is covered by the KUnit cases
  in `drivers/thunderbolt/test.c` (core). Run via
  `drivers/thunderbolt_frame/tools/run-kunit.sh` (`kunit.py`, x86_64 qemu).
- Hardware (2026, legacy era): with the patched core on both ends of a link, a
  soft RDMA-driver reload re-negotiates without a reboot (the patched side
  re-reads where an unpatched neighbour's gen-gate blocks it).

### Ring-0 dispatch lock invariant

Ring 0 is both the receive path for XDomain service packets and the completion
path for synchronous ICM/configuration requests. A service packet must not do a
blocking topology lookup or invoke a service callback directly from ring-0
completion context.

The failure cycle is:

1. a management or sysfs operation holds the domain topology mutex and waits
   synchronously for its controller response;
2. ring 0 receives an unrelated service packet and blocks on that same mutex
   while resolving its source route;
3. the response from step 1 is queued behind the blocked ring worker, so the
   mutex owner and ring worker wait for one another;
4. frame-session requests then accumulate behind the stalled completion path,
   eventually appearing as late unmatched replies after teardown.

Discovery requests and non-discovery service **requests** are therefore copied
and deferred before any topology mutex, XDomain mutex, or protocol-handler
dispatch lock is acquired. Service **responses** must instead remain
unconsumed so `tb_cfg_request_find()` can deliver them to the already-armed
peer-response waiter. Deferred service dispatch retains the domain for the work
item's lifetime and preserves the existing handler-unregistration fence around
callbacks.

This distinction is also the explanation for the superficially conflicting
wire values observed during the failure. Packet type `0xc` is the controller's
local completion for a transmitted command; packet type `0x7` is the remote
host's end-to-end XDomain response. Both arrived. The first deferred-dispatch
implementation incorrectly queued the `0x7` response as handler work and
reported it consumed before the request matcher ran, converting every valid
HELLO acknowledgement into an apparent timeout.

`tb_test_xdomain_request_and_response_dispatch_are_separate` models the same
request/response classifier used by the production receive path. Its RED run
had 197 passing cases and one failure because a service response was deferred;
its GREEN run has all 198 cases passing, requiring service requests to be
deferred and service responses to remain matcher-owned. The existing ABI and
handler-unregistration tests exercise the callback walk independently of
scheduling so they continue to verify source-blind compatibility and unload
fencing.

### Control recovery needs overlapping evidence

Announcement retries and inbound control reception are independent state
machines.  A route-correlated inbound request proves that the peer was present
when that request arrived; it does not prove that the peer is still present
minutes later when a property announcement reaches saturated backoff.

The first directional-control detector retained an inbound timestamp for five
minutes.  During a coordinated reload, a peer could send one request and then
unbind.  The surviving endpoint later combined that stale timestamp with an
outbound timeout, declared a one-way controller failure, and reset its entire
two-port domain.  That removed an unrelated healthy sibling and allowed the
failure to cascade along a chain.

Production now samples `request_started` immediately before each bounded
outbound request.  Controller recovery is eligible only if a route-correlated
inbound request was observed at or after that sample.  Old peer activity still
keeps the announcement retrying, but cannot justify a destructive domain
recovery.  The wrap-safe predicate lives in the shared negotiation header.

This was modeled RED before changing production.  The new stale-inbound case
was the only failure in the 199-test core suite: the old model incorrectly set
`recovery_requested`.  The independent model now distinguishes historical
inbound activity from activity overlapping the failing request; the shared
production predicate is also checked at the boundary.  The core suite then
passed 199/199 and the selected Thunderbolt suites passed 230/230.

### ICM responses precede path stability

An ICM command response proves that firmware accepted the command, but it does
not prove that the corresponding XDomain path context is already stable. A
previously developed settle fix was absent from a later integration branch,
leaving approve with no post-response delay and only 10--50 microseconds
between the two disconnect stages. Under simultaneous peer negotiation, both
control state machines could complete HELLO and READY while the resulting DMA
paths delivered no frames.

The production transition model now separates approve response, approve
settle, active, disconnect stage-one response and settle, stage-two readiness,
stage-two response and settle, and inactive. Each firmware response holds the
global ICM request lock across a 2--2.5 millisecond settle. This prevents
another command from observing an intermediate path context and prevents a
consumer from treating response arrival as path activation.

The focused KUnit was run RED against the direct-response behavior before the
production request path changed. It observed `ACTIVATE` immediately after an
approve response where `SETTLE` was required. After wiring the transition
model into the real ICM request stream, the settle-order and crossed-stage
tests both passed.

### Properties-changed success responses are 32 bytes

An XDP success packet and an XDP error packet are alternative wire layouts,
not two fixed-size instances of the same C union. In particular, a successful
`PROPERTIES_CHANGED_RESPONSE` is exactly `struct tb_xdp_header` (32 bytes),
whereas the error alternative has an additional error dword (36 bytes).

A strict response matcher incorrectly used
`sizeof(struct tb_xdp_properties_changed_response)`, the size of the Linux
success/error receive union, as the minimum valid success size. It therefore
rejected a canonical 32-byte peer response even when its route, sequence,
declared size, UUID and response type all matched. The sender made the inverse
mistake and emitted the union size for successful responses. A trace presents
this as a successful local command completion (`0xc`) followed promptly by a
valid peer response (`0x7`) that Linux marks dropped; the later request timeout
is a host-side matching error, not evidence that either packet was absent.

Production now uses the common header size for both the success matcher and
success sender. The independent XDP size model encoded the canonical 32-byte
layout first; the focused RED run rejected that packet, and the GREEN run plus
the complete suite accepts it. The other native XDP success/error unions were
audited individually; their success arms are not smaller than their error
arms, so this size inversion is specific to properties-changed.

### Runtime control and data recovery require distinct proofs

Controller recovery has one serialized executor because unbinding, resetting
and reprobeing a PCI function cannot run concurrently. Its evidence remains
two independent state machines:

- A control-recovery episode begins only from overlapping one-way XDomain
  evidence and completes only when route-correlated control traffic succeeds
  after reprobe.
- A data-recovery episode begins only from a stalled hardware consumer or an
  unresolved DMA-path handoff and completes only after the frame service proves
  both directions of the current DMA session and observes a real TX descriptor
  completion.

The original implementation persisted state and route across the driver
rebind, but not the episode kind. Worse,
`tb_nhi_runtime_control_path_proven()` called the data-proof entry point. An
ordinary properties-changed exchange could therefore delete a verifying data
episode before any DMA frame crossed. If the DMA path remained broken, its
next verification failure created a new episode and unbound the controller
again. Each individual episode was bounded, but the incorrect proof reopened
the episode boundary and produced an unbounded unbind/reprobe loop.

Runtime records now persist data/control kind with route and state. Proofs and
post-recovery failures must match all three before they may complete or poison
an episode. Cross-kind evidence is left for its own state machine and is logged
with both kinds and routes, while probe/remove diagnostics identify the exact
proof being awaited. KUnit modeled both contamination directions before the
queue and proof implementations changed: control proof completing data
recovery and control failure poisoning data recovery each failed RED, then
remained `VERIFYING` after the fix.

See also: `../../icm-firmware-re/` (ICM disassembly), the memory note
`project-tb-xdomain-renegotiation-bug`, and `scripts/tb-chain-reboot-cover.sh`
(the reboot workaround this replaces).
