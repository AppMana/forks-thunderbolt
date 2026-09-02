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

See also: `../../icm-firmware-re/` (ICM disassembly), the memory note
`project-tb-xdomain-renegotiation-bug`, and `scripts/tb-chain-reboot-cover.sh`
(the reboot workaround this replaces).
