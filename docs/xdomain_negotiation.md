# XDomain negotiation and recovery architecture

`thunderbolt` owns topology and properties; `thunderbolt_net` provides
IP-over-Thunderbolt; `thunderbolt_frame` provides frame transport and RXE.
The retired `thunderbolt_ibverbs` package must not be installed.

PDF `0xc` is local serialized ring-command completion. PDF `0x7` is a peer
XDomain response matched independently by route and sequence. Service requests
are copied and deferred before topology locks; service responses remain owned
by the armed matcher. Teardown cancels and flushes both machines.

Firmware announcements are edge-triggered. Every reconnect signal must clear
the cached remote generation, reset the service handshake, remove stale service
children, and re-read/re-probe properties. The write-only recovery trigger is:

```
echo 1 | sudo tee /sys/bus/thunderbolt/devices/<xdomain>/rescan
```

A cached generation of zero forces acceptance of the next real property block.
Control evidence proves only that XDomain messages move. Data recovery requires
route-specific authenticated traffic in both directions after reprobe; stale
inbound activity cannot justify destructive recovery of a later request.

ICM command responses require explicit path-settle states before the next
stage. Successful `PROPERTIES_CHANGED_RESPONSE` packets are exactly the 32-byte
XDP header; the larger error form must be validated separately.

Record ring progress, authenticated frame counters, peer RX deltas,
route/generation, and AER/DPC counters. A five-minute bidirectional soak is
the acceptance test; carrier and `LOWER_UP` are not proof.
