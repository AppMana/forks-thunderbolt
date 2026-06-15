# Thunderbolt XDomain connection negotiation — shared model and the soft-reconnect fix

This documents the connection-negotiation logic shared by the three Thunderbolt
host-to-host stacks we run, the bug they share, and the single common header
that fixes it.

- `thunderbolt` — the core driver (XDomain property exchange).
- `thunderbolt_net` — IP-over-Thunderbolt (`tbnet`).
- `thunderbolt_ibverbs` — the out-of-tree usb4_rdma RDMA transport.

## The shared negotiation model

All three negotiate a host-to-host connection in the same two layers, over the
same XDomain control channel (ring 0, `TB_CFG_PKG_XDOMAIN_REQ/RESP`):

1. **Property/identity exchange (owned by the core).** Each host advertises an
   XDomain *property directory* via `tb_register_property_dir()`
   (`thunderbolt_net` registers `"network"`, `thunderbolt_ibverbs` registers
   `"tbverbs"`). The remote block carries a **monotonic generation**. A peer
   re-reads the remote directory and accepts it only if the generation is
   strictly newer (`tb_xdomain_get_properties()` in `xdomain.c`). Service
   discovery (`enumerate_services()`) then binds the matching service driver
   and fires its `.probe`.

2. **A login/HELLO handshake (owned by each service driver).** On top of the
   discovered service, each driver runs a one-shot two-phase handshake:
   - `thunderbolt_net`: `login_sent` / `login_received` / `login_retries`
     (`start_login()`, `tbnet_login_work()`, `tbnet_login_request/response()`).
   - `thunderbolt_ibverbs`: `native_ready_sent` / `native_remote_ready` /
     `native_negotiated` / `native_ready_attempts` (`native_control.c`).
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

`drivers/thunderbolt/thunderbolt_negotiation.h` carries the shared primitives,
vendored byte-identically into `thunderbolt_ibverbs` (`proto/`). Keep the copies
in lockstep.

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
- `thunderbolt_ibverbs`: replace the per-rail `native_ready_sent` /
  `native_remote_ready` / `native_negotiated` booleans with the shared struct
  and reset on reconnect; consume the core `rescan` so a single sysfs write
  recovers a stuck rail.

## Validation

- Pure predicate `tb_xdomain_generation_stale()` is covered by the KUnit case
  `tb_test_xdomain_generation_stale` (`test.c`) and a userspace mirror
  (`tests/xdomain_properties_userspace.c`) — fleet kernels lack `CONFIG_KUNIT`.
- Hardware: with the patched core on both ends of a link, a soft
  `thunderbolt_ibverbs` reload re-negotiates without a reboot (the patched side
  re-reads where an unpatched neighbour's gen-gate blocks it).

See also: `../../icm-firmware-re/` (ICM disassembly), the memory note
`project-tb-xdomain-renegotiation-bug`, and `scripts/tb-chain-reboot-cover.sh`
(the reboot workaround this replaces).
