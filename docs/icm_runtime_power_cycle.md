# ICM runtime power-cycle recovery

## Status

The Linux runtime-recovery path for Intel Maple Ridge controllers omitted the
controller preparation performed by Intel's Windows driver before it asks ICM
firmware to power-cycle the host router. The omission allowed Linux to report a
power cycle as dispatched after only the local DMA config-write transaction had
been consumed. It did not establish that the controller could carry out the
requested transition.

The implementation in this tree adds the missing preflight and makes it a hard
prerequisite for runtime dispatch. Its state and ordering contracts are covered
by red-first KUnit tests. Hardware validation must still prove bidirectional
payload traffic; link enumeration, service negotiation, `LOWER_UP`, and local
TX completion are not accepted as data-path proof.

## Observable failure

The failing link has a deceptively healthy control plane:

- both host routers and the remote XDomain enumerate;
- the frame service exchanges properties, negotiates, and approves its paths;
- outbound producer and descriptor-completion counters move; and
- a network interface can report carrier and `LOWER_UP`.

Despite that, peer receive counters remain at zero and authenticated payloads
do not cross in either direction. Reloading the Linux modules reconstructs host
objects but does not necessarily repair the controller's ARC/CIO and tunnel
state.

This is not specific to `thunderbolt_frame`. An A/B test with the distribution's
unmodified Thunderbolt core and `thunderbolt_net` reproduced the same result:
both endpoints reported carrier, both transmitted, neither received. That
places the persistent fault below the frame service and shows that a carrier
bit derived from tunnel setup is not proof of a working tunnel.

## Evidence hierarchy

The diagnosis deliberately separates facts from inference.

Established by live observation:

1. The XDomain control plane can remain functional while the data plane is
   inert.
2. Both the frame transport and stock `thunderbolt_net` reproduce zero receive
   progress on the same controller state.
3. A module unload/reload alone does not recover that state.
4. Before this change, the relevant Maple Ridge systems had all three Windows
   preflight bits clear.

Established by the Intel Windows binary:

1. A runtime host-router power cycle is a sequence, not a single mailbox write.
2. The sequence first changes two PCIe-upstream-proxy registers and one root
   router register.
3. Any failed preparation step aborts the operation before power-cycle command
   dispatch.
4. The subsequent command writes the DMA-port power-cycle mailbox and uses a
   25-second command timeout.

The remaining hypothesis to prove on hardware is that performing this sequence
lets the controller execute a real recovery transition and restores payload
traffic. A consumed config write alone cannot prove that conclusion.

## The Windows controller sequence

The following functions are in Intel `TbtBusDrv.sys` 1.41.1193.0. Addresses are
image virtual addresses from the complete decompilation.

`HostControllerPowerCycle::RunWorkaroundNoOsReenumerationOnPowerCycle`
(`FUN_140239048`, `0x140239048`) invokes the preparation in this order:

1. `HostControllerPowerCycle::DisalbeAndExitL1`
   (`FUN_140237678`, `0x140237678`):
   - read upstream-proxy dword index `0x143`, set `BIT(3)`, and write it back;
   - read upstream-proxy dword index `0x175`, set `BIT(19)`, and write it back.
2. When the controller policy enables the workaround,
   `HostControllerPowerCycle::EnablePlugEventDelay`
   (`FUN_140237d68`, `0x140237d68`):
   - read root-router config dword `0x13e`;
   - set `BIT(21)` and write the dword back.

Only after that succeeds does `HostControllerPowerCycle::RunPowerCycle` create
and send the command. `PowerCycleCommand::Send` (`FUN_140171d60`,
`0x140171d60`) constructs the value `0x40000001` and issues the DMA-port config
write at mailbox dword `0x4f`, with a 25-second timeout.

The upstream-proxy API addresses registers by dword index. Linux PCI config
accessors address bytes, so the corresponding offsets are `0x143 * 4` and
`0x175 * 4`. Root-router config space is already addressed in dwords by
`tb_sw_read()` and `tb_sw_write()`.

## The Linux defect

The old runtime path in `domain.c` called `dma_port_power_cycle_raw()` directly.
That helper can determine whether the outbound config transaction was consumed,
but consumption is only a local command-layer event. The path had no
controller-specific preflight and therefore could skip the state that makes a
Maple Ridge runtime power cycle executable.

The old success vocabulary was consequently too strong:

```
DMA config write consumed -> power cycle dispatched -> recovery assumed
```

The defensible interpretation is:

```
DMA config write consumed -> local command left the host successfully
```

It does not prove that firmware accepted the semantic operation, that the host
router changed generation, that the PCI function disappeared and returned, or
that a repaired tunnel carries data.

### First live implementation exposed a second ordering bug

The first preflight implementation correctly placed all preparation before the
mailbox write, but invoked both from the old late reset slot. Domain removal had
already called `prepare_xdomains()` by then. That function recursively marks the
root switch and its children unplugged before service removal. `tb_sw_read()`
correctly refuses config access through an unplugged switch, so the root-router
preflight returned `-ENODEV` and the safety gate prevented mailbox dispatch.

The hardware register delta proves where it stopped. On both endpoints,
upstream dword `0x143` changed from `0x081806a1` to `0x081806a9` and dword
`0x175` changed from zero to `0x00080000`. The upstream bridge was therefore
found and both PCIe writes completed. No success message or mailbox dispatch
followed; the next root-router access returned `-ENODEV`.

This was converted into a second red KUnit before changing the sequencing. The
test expected preflight between callback quiesce and topology invalidation, but
the old production order recorded only 11 events and went directly from deinit
to topology preparation. The corrected path splits preparation from command
dispatch:

```
stop notification admission
  -> synchronously drain callbacks
  -> controller preflight while root config is valid
  -> invalidate and unregister topology/services
  -> dispatch mailbox command
```

The companion failure test proves that a failed early preflight still completes
ordinary teardown but never reaches mailbox dispatch.

## Why PDF `0xc` and PDF `0x7` are not contradictory

The Intel Windows stack models XDomain communication with two independent state
machines. Combining them loses real event order and creates false timeouts or
false success.

### Local ring-command state machine: PDF `0xc`

Every outbound XDomain packet enters the globally serialized
`RingCommandSender`. Exactly one command owns the unsequenced local completion.
For XDomain sends that completion arrives as PDF `0xc`. It says whether local
ICM firmware accepted the send operation; status bit 8 reports rejection.

The machine is approximately:

```
IDLE -> CURRENT_COMMAND -> LOCAL_COMPLETE -> IDLE
                         -> LOCAL_TIMEOUT  -> IDLE
```

Timeout and completion both release the current owner and advance the global
queue. Stop synchronously cancels the timer, completes the current command,
flushes queued commands, and flushes completion work.

### Peer-protocol event state machine: PDF `0x7`

A request can independently produce a response from the remote host as PDF
`0x7`. `FirmwareEventsDispatcher` owns that packet and routes it to the
XDomain connection by route. It is not armed after PDF `0xc`; it is already
live and may deliver the peer response while the sending thread is still
waiting for its local completion.

The machine is approximately:

```
PEER_WAITER_ARMED -> PEER_RESPONSE_DISPATCHED
                  -> PEER_TIMEOUT/CANCELLED
```

It has its own stop operation and work flush. Therefore:

- PDF `0xc` means local send completion, not the remote reply;
- PDF `0x7` means a peer protocol response, not local DMA completion;
- their arrival order is not a correctness ordering; and
- the two machines must be fenced separately during teardown.

The `0xc`/`0x7` discrepancy was a clue to missing state separation, but is not
itself a malformed-controller response.

## Correct runtime recovery model

Runtime recovery now follows this state sequence:

```
DATA_FAILED
  -> QUIESCED
  -> PCIE_L1_EXIT_PREPARED
  -> ROOT_PLUG_EVENT_DELAY_ENABLED
  -> MAILBOX_COMMAND_CONSUMED
  -> CONTROLLER_REPROBE_REQUIRED
  -> DATA_PROVEN
```

Any preflight error terminates recovery before mailbox dispatch. No later state
may be inferred from an earlier one. In particular, `MAILBOX_COMMAND_CONSUMED`
is not equivalent to `DATA_PROVEN`.

The implementation adds a controller-manager operation named
`prepare_runtime_power_cycle`. Maple Ridge wires it to an ICM implementation
that:

1. locates the supported Intel upstream bridge;
2. read-modify-writes the two PCIe extended-config registers;
3. read-modify-writes root-router config dword `0x13e`; and
4. returns success only after all three writes succeed.

The domain recovery code calls this operation after stopping and draining ICM
callbacks but before topology invalidation. It removes services and releases
their data paths after the preparation attempt, then sends the raw DMA mailbox
command only when preparation succeeded. A missing operation returns
`-EOPNOTSUPP`; any read or write failure is returned to the caller, ordinary
teardown continues, and dispatch is skipped.

The existing startup recovery remains separate. Startup power cycling is used
when root-router config access itself failed, so it cannot require a successful
root-router config preflight without making the only recovery path impossible.
The new preflight applies to runtime recovery, where the root router is still
available and Linux is trying to repair a stale data plane.

## Red-first KUnit model

The production paths use injected operation tables so KUnit tests execute the
same ordering logic without pretending that a PCI config write is real
hardware.

The initial domain test was run before production called preflight. It failed
because `preflight_calls` was zero and dispatch observed no completed
preflight. The production change made it green.

The tests now enforce:

- preflight occurs exactly once after callback quiesce, before topology
  invalidation, service removal, and mailbox dispatch;
- successful local command consumption maps only to the existing dispatched
  recovery state;
- preflight failure returns the original error and performs zero dispatches;
- ICM preparation orders exit-L1, root read, then root write; and
- the root write preserves existing bits while setting `BIT(21)`.

This test shape avoids a cheating test: expected state transitions existed
before the production call was added, and the test invokes the shared
production sequencing helper rather than a duplicated test-only algorithm.

## Hardware validation and acceptance criteria

The fix is accepted only when all of the following hold:

1. The driver logs successful completion of all preflight steps before mailbox
   dispatch.
2. Register reads confirm the requested bits, rather than relying on the log.
3. Recovery produces an observable controller transition or a new controller
   generation; a TX-consumed result alone is insufficient.
4. The XDomain and service re-enumerate without stale object reuse.
5. Authenticated payload traffic crosses in both directions and both peer RX
   counters increase.
6. A five-minute bidirectional soak has no data-path stall, unmatched command
   escalation, AER/DPC growth, refcount failure, use-after-free, or teardown
   warning.
7. Coordinated unload/reload leaves no command owner, event work, service,
   XDomain, path, or ring behind.

If the preflight bits are set but no controller transition occurs, the next bug
is command-completion proof: runtime recovery must wait for a hardware-visible
generation/disappearance transition instead of treating config-write
consumption as completion. That case must be modeled red in KUnit before adding
another production transition.

## General test procedure

Run the complete Thunderbolt KUnit set, then repeat the focused recovery suites
with lockdep enabled. Build all three shipped modules against the target kernel.
On two otherwise idle endpoints, collect register and counter baselines, perform
a coordinated hot reload, allow recovery to run, and verify bidirectional
authenticated traffic. Continue that traffic for five minutes while sampling
driver diagnostics, PCIe AER/DPC counters, ring progress, and peer RX counters.

For an A/B, use a matching distribution kernel core and `thunderbolt_net` from
one module tree. Do not mix module builds or use `LOWER_UP` as the result. The
result is the payload receive delta on both endpoints.
