# ICM runtime power-cycle recovery

## Status

The Linux runtime-recovery path for Intel Maple Ridge controllers omitted the
controller preparation performed by Intel's Windows driver before it asks ICM
firmware to power-cycle the host router. The omission allowed Linux to report a
power cycle as dispatched after only the local DMA config-write transaction had
been consumed. It did not establish that the controller could carry out the
requested transition.

The implementation in this tree adds the missing preflight, performs its
root-router commands with the sequence identity observed in Intel's driver,
and makes successful preparation a hard prerequisite for runtime dispatch. Its
state, ordering, and exact packet-encoding contracts are covered by red-first
KUnit tests. A live test disproved the narrower hypothesis that sequence
identity was the only wire-level mismatch: the controller consumed the
sequence-one write and still rejected it as an invalid configuration-space
operation. The preflight is therefore a useful safety boundary and diagnostic,
but this revision is not a completed recovery fix.

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
5. With the preflight ordered while topology was still valid, both PCIe proxy
   writes and the root-router read completed, but the root-router write failed
   immediately with `-EIO`.
6. Repeating only the root write through debugfs on a bound, otherwise intact
   controller failed in the same way. Register `0x13e` remained readable and
   retained value zero. Teardown timing, service removal, and an unplugged
   software object therefore cannot explain this failure.
7. With the Windows-shaped sequence-one address deployed, the read still
   completed but the write returned a protocol error with `err=1`,
   `tb_error=2`, response route and port zero, and request-local TX state
   `CONSUMED`. Error 2 is `TB_CFG_ERROR_INVALID_CONFIG_SPACE`. This proves the
   request reached the controller and falsifies sequence identity as a
   sufficient repair.

Established by the Intel Windows binary:

1. A runtime host-router power cycle is a sequence, not a single mailbox write.
2. The sequence first changes two PCIe-upstream-proxy registers and one root
   router register.
3. Any failed preparation step aborts the operation before power-cycle command
   dispatch.
4. The subsequent command writes the DMA-port power-cycle mailbox and uses a
   25-second command timeout.
5. Both device-configuration mail commands used for register `0x13e` set
   address bit 27, which is sequence identity 1 in Linux's
   `tb_cfg_address` layout.
6. The Maple Ridge `8086:1137` device policy explicitly enables the
   plug-event-delay workaround; it is not an optional step Linux can skip on
   this controller.

The unresolved question is what semantic distinction Windows' device-config
`MAIL` operation has from Linux's native ring-zero configuration write, or
what controller precondition Linux has not reproduced. A consumed config write
alone cannot prove either acceptance or recovery.

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

For Maple Ridge NHI device `8086:1137`, that policy is enabled. The complete
constructor path is `FUN_140146b60` -> `FUN_140144738` -> `FUN_140048254` ->
`FUN_140048c54`. `FUN_140048254` passes one as argument 12;
`FUN_140048c54` stores it at device-info offset `0x7c` and emits the label
`Enable plug event delay for power cycle`. `HostControllerPowerCycle::Init`
(`FUN_1402383bc`) copies offset `0x7c` to the power-cycle object's offset 300,
and `FUN_140239048` tests that byte before calling `FUN_140237d68`. The
`0x1137` device-table entry reaches this constructor path. Consequently,
omitting register `0x13e` would diverge from the controller-specific Windows
policy rather than avoid a questionable workaround.

Only after that succeeds does `HostControllerPowerCycle::RunPowerCycle` create
and send the command. `PowerCycleCommand::Send` (`FUN_140171d60`,
`0x140171d60`) constructs the value `0x40000001` and issues the DMA-port config
write at mailbox dword `0x4f`, with a 25-second timeout.

The name `PciUpstreamProxy` is literal and important: it is not an ordinary
PCI configuration accessor for the host CPU's upstream port. Its complete read
and write implementations are `FUN_1401f80d0` and `FUN_1401f9240`. They use
three root-router device-config dwords as an indirect PCIe bridge mailbox:

- dword `0x43`: PCIe write data;
- dword `0x44`: command and status; and
- dword `0x45`: PCIe read data.

For a read, Windows writes a command to `0x44`, polls `0x44` until request bit
30 clears, rejects timeout bit 31, then reads the result from `0x45`. For a
write, it first writes the value to `0x43`, writes the command to `0x44`, and
performs the same poll. The command encodes the target dword in bits 9:0,
selects internal upstream bridge zero with bit 10, uses command value two in
bits 24:22, selects write with bit 21, and requests execution with bit 30.
Consequently the four commands used by the workaround are:

```
read  0x143 -> 0x40800543
write 0x143 -> 0x40a00543
read  0x175 -> 0x40800575
write 0x175 -> 0x40a00575
```

This format is also recognizable in Linux's existing Thunderbolt plug-events
PCIe bridge proxy definitions. It is a controller transaction carried by root
router configuration space, not a byte-versus-dword translation for
`pci_read_config_dword()`.

### Exact device-config packet construction

The decompiled read and write command implementations independently construct
the same 32-bit address word:

- `ReadConfigurationRegistersCommand::Send` (`FUN_14028e564`) assigns the
  dword index to bits 12:0, length to 18:13, DMA port to 24:19,
  configuration space to 26:25, and then unconditionally sets bit 27.
- `WriteConfigurationRegistersCommand<...MAIL>::Send`
  (`FUN_14028e370`) derives length from the payload, assigns all the same
  fields, and likewise unconditionally sets bit 27.

For route zero, device configuration space, port zero, one dword, and dword
index `0x13e`, the third request dword is therefore:

```
offset 0x13e | length (1 << 13) | space (2 << 25) | sequence (1 << 27)
  = 0x0c00213e
```

Linux defines bits 28:27 as the two-bit `seq` field. This is not a new or
undocumented packet flag: the difference is sequence 1 versus sequence 0.
The Windows read wrapper (`FUN_14027f100`) and synchronous write wrapper
(`FUN_140173178`) each use a 25-second timeout for this command stream.

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

### Correct ordering exposed two more wire-level bugs

After the ordering fix, a live runtime-recovery attempt progressed farther:

```
PCIe proxy dword 0x143 write succeeds
  -> PCIe proxy dword 0x175 write succeeds
  -> root dword 0x13e read succeeds
  -> root dword 0x13e write returns -EIO immediately
  -> preflight fails
  -> mailbox dispatch count remains zero
```

An isolated debugfs write reproduced the immediate failure while the root
router was still bound and readable. That experiment falsified the theory that
the write failed because domain teardown had already removed the service or
invalidated topology.

One confirmed mismatch was in the request address dword. Generic Linux config
I/O starts a fresh request stream at sequence zero. Its first packet for this
write was `0x0400213e`. Intel's device-config mail command sends sequence one,
making the same packet `0x0c00213e`. Generic Linux retries advance the sequence
only after `-ETIMEDOUT`; a controller error response terminates the loop. An
immediate rejection of sequence zero can therefore never fall through to the
sequence-one packet by accident.

The sequence experiment does not change the convention for ordinary router, port,
path, or counter access. The runtime power-cycle preflight now owns a
single-request command stream, explicitly selects sequence one for both the
read and the write, and uses the Windows command's 25-second timeout. The raw
result is logged before errno translation, including `err`, `tb_error`, reply
route and port, request-local TX state, and sequence. This distinguished the
live sequence-one result from a timeout, shutdown, or local-send failure: it
was an immediate controller protocol rejection. Consequently the sequence
change records a real Windows/Linux difference but is not, by itself, the
repair.

The next complete-code comparison found the more fundamental defect. Linux had
translated `PciUpstreamProxy::ReadPcieUpstreamRegister(0x143)` into
`pci_read_config_dword(upstream, 0x143 * 4)` and similarly translated `0x175`.
That changed the ordinary host PCI bridge, not the controller's internal PCIe
upstream bridge. The calls returned success, which made the incorrect target
look like a completed precondition. Live readback showed the two wrong host
registers changed from `0x081806a1` to `0x081806a9` and from zero to
`0x00080000`. Those exact bits were later cleared on the experiment endpoints;
readback confirmed the original values were restored.

The incorrect direct accesses also explain the otherwise puzzling next event:
the root-router `0x13e` write was still rejected because the controller-side
L1 precondition had never been applied. The corrected implementation performs
the complete root-router `0x43`/`0x44`/`0x45` proxy transactions and checks both
request completion and the proxy timeout bit before proceeding to `0x13e`.
This is the current hardware hypothesis; it is not called a fix until the live
controller accepts the entire preflight and bidirectional payload traffic
passes the acceptance criteria below.

## Why PDF `0xc` and PDF `0x7` are not contradictory

The `0x0c00213e` address dword above is unrelated to PDF `0xc`. The former is a
packed configuration address whose high byte happens to begin with `0x0c`; the
latter is a control-ring packet type.

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
  -> PCIE_PROXY_READ_0x143_COMPLETE
  -> PCIE_PROXY_WRITE_0x143_COMPLETE
  -> PCIE_PROXY_READ_0x175_COMPLETE
  -> PCIE_PROXY_WRITE_0x175_COMPLETE
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

1. encodes the controller's internal upstream-bridge proxy commands;
2. read-modify-writes the two PCIe registers through root device-config
   dwords `0x43` through `0x45`, polling each command to a terminal state;
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

### Failed preflight must not strand the PCI function

Fleet-wide hot reload exposed a recovery failure after otherwise successful
service discovery.  Every affected peer completed HELLO and READY, but a
directional data proof found a transmit path with no authenticated peer
acknowledgement.  The frame service correctly asked the controller recovery
machine for help.  The ICM upstream-proxy write then returned a firmware error,
so the host-router power-cycle preflight correctly refused to dispatch the
mailbox command.

The destructive result came from a later state transition.  Recovery runs from
`device_release_driver()`, so by the time preflight reports failure the domain
and PCI driver have already been detached.  The old transition changed
`POWER_CYCLE_PENDING` directly to `POWER_REQUIRED`.  The recovery worker only
reattached a driver in `REPROBE_PENDING`, leaving the live PCI function
permanently unbound.  On a two-link host this removed both neighbors, and their
own data-proof recovery could repeat the failure outward as a cascade.

A rejected runtime power-cycle now enters `REPROBE_PENDING`.  This is a bounded
fallback, not a claim that firmware reset anything: the driver binds once,
reconstructs the controller domain, and enters `VERIFYING`.  Only authenticated
data on the exact failed route completes the episode.  Reprobe failure or a
second failure of that route remains terminal and requests power removal.

The state-machine KUnit was changed first and run against the unchanged
production transition.  It produced one failure in the 198-test core suite:
the observed state was `POWER_REQUIRED` while the expected state was
`REPROBE_PENDING`.  After changing the shared transition used by production,
all 198 core tests and all 229 tests selected by the Thunderbolt suite glob
passed.

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
- the root write preserves existing bits while setting `BIT(21)`; and
- the shared production sequence selector packs the exact device-config
  address `0x0c00213e`, rather than the rejected generic encoding
  `0x0400213e`;
- the PCIe proxy command encoder produces the four exact Windows command
  values above rather than host PCI byte offsets; and
- the proxy state machine orders command, poll, and read-data operations,
  orders write-data before a write command, and rejects the target-timeout bit.

For the packet test, production initially still selected the generic first
attempt. The focused KUnit emitted two failures:

```
address.seq: expected 1, observed 0
encoded: expected 0x0c00213e, observed 0x0400213e
```

Only after that red result was captured did production switch the two
preflight requests to its controller-specific one-request stream. The same
test then passed. The test builds a real `tb_cfg_address` using the sequence
selector called by production and copies its packed dword; it does not use a
separate test encoder.

The upstream-proxy test was likewise run before replacing the direct PCI
implementation. It failed all four cases:

```
0x143 read:  expected 0x40800543, observed 0x0000050c
0x143 write: expected 0x40a00543, observed 0x0000050c
0x175 read:  expected 0x40800575, observed 0x000005d4
0x175 write: expected 0x40a00575, observed 0x000005d4
```

The observed values are exactly `index * 4`, proving that the old model was a
host PCI byte-offset conversion. Only after recording that red result was the
production helper changed to emit the controller proxy command. The focused
suite then passed eight tests, and the complete suite passed all 316 tests.

This test shape avoids a cheating test: expected state transitions existed
before the production call was added, and the test invokes the shared
production sequencing helper rather than a duplicated test-only algorithm.

## Hardware validation and acceptance criteria

The eventual fix is accepted only when all of the following hold:

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
