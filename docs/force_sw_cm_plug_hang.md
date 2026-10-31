# Software connection-manager safety

`force_sw_cm=1` is experimental. Use it only on an isolated, drained host
with a one-shot boot entry containing `thunderbolt.force_sw_cm=1`,
`thunderbolt.clx=0`, and `pcie_ports=native`. Keep the normal GRUB entry as the
default and arm netconsole plus a supervised watchdog before a plug test.

The software reconcile loop must not infer unplug from one lane-state sample.
If an enumerated router answers a bounded config read, retain the live link
even when the lane register says `UNPLUGGED`; only failed reachability may
synthesize unplug. A failed lane read ends the pass instead of holding
`tb->lock` through repeated timeouts.

Use dynamic debug, netconsole, NMI backtraces, PCIe AER/DPC counters,
Thunderbolt tracepoints, ring producer/consumer counters, and peer RX deltas.
Carrier and `LOWER_UP` are not payload proof. Accept only authenticated traffic
in both directions during a five-minute soak.

For a supervised plug test, drain one host, start a sink on another host, boot
with cables disconnected, record controller/topology and PCIe hotplug ownership,
connect one cable at a time, and preserve all logs. Convert any observed
operation sequence into mock-firmware KUnit before changing the reconcile policy.
