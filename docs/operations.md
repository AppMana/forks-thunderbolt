# Thunderbolt stack operations

Keep source fixes in this repository and package them for the target
distribution. Do not put infrastructure-specific host names, credentials, or
run diary text in source comments or commit messages.

## Official deployment

Use the live apt repository and Ansible to install matching versions of:

```
thunderbolt-tbfix-dkms
thunderbolt-frame-dkms
usb4-rdma-provider
```

The frame package contains both `thunderbolt_frame` and
`thunderbolt_frame_rxe`, and depends on the exact core version. Never install
`thunderbolt_ibverbs` or mix module builds. Verify:

```
modinfo thunderbolt thunderbolt_frame thunderbolt_frame_rxe | grep -E '^(filename|version|vermagic):'
dkms status
```

Install packages without rebooting when the host policy permits it. Core
replacement becomes active at the next controlled reboot; frame/RXE reloads
require quiescing dependents and are not a substitute for resetting wedged ICM
firmware.

## Ad-hoc build and staging

From `~/Documents/forks-thunderbolt`, build against the target kernel with:

```
./scripts/oot-build.sh --build
./scripts/oot-build.sh --install   # next-boot install only
```

For a live experiment, isolate the selected endpoint pair, stop users of the
modules, unload in dependency order, and load a matching set. Preserve a normal
boot path. If ICM stops answering after an unload, use a cold reboot; continuing
to poke a wedged controller obscures the failure.

## Instrumentation

Use layered evidence: dynamic debug/netconsole/NMI watchdog for hangs;
Thunderbolt tracepoints and NHI ring producer/consumer indices for DMA;
raw config responses (`err`, `tb_error`, route, port, TX state, sequence) for
ICM protocol; `lspci -vv` plus AER/DPC counters for PCIe; and peer RX deltas for
actual payload proof. A VM with PCIe Thunderbolt-root ownership is suitable for
repeatable protocol experiments. KUnit should model packet encodings, two
completion state machines, lock ordering, teardown, and red/green recovery
transitions before production edits.

## Acceptance and rollback

Return a host to service only after both directions carry authenticated payloads
for five minutes with advancing RX/TX counters and no ring stall, AER/DPC
growth, unmatched-response escalation, or teardown warning. Carrier and
`LOWER_UP` do not qualify. Roll back through apt/Ansible to the last matching
package set or boot entry, preserving raw logs and counters with the test
artifact.
