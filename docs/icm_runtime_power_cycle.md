# Maple Ridge data-path-dead state and the power-cycle primitive

On Maple Ridge `8086:1137`, XDomain control traffic can complete HELLO/READY
while the DMA data plane is inert. NHI RX rings may be fully posted while the
hardware RX producer never advances. Stock Thunderbolt networking can reproduce
this shape, so it is below `thunderbolt_frame`. The control ring being alive is
not evidence the data engine is: only advancing peer RX counters in both
directions prove a live path.

## There is no runtime software recovery for this state

The driver does **not** attempt to power-cycle a live host router at runtime,
and does not unbind/rebind the domain to recover a stalled data path. Earlier
revisions did, modelled on the reference driver's `HostControllerPowerCycle`
sequence (proxy `0x143` bit 3, `0x175` bit 19, root dword `0x13e` bit 21, then
the DMA-port power-cycle mailbox on the DMA port). That was a misapplication:
the reference driver only issues that sequence as part of an NVM image write or
to leave CM safe mode, never as a data-path recovery. On a controller in a
normal running state Maple Ridge rejects the preparation writes (`err=1`,
`tb_error=2` `TB_CFG_ERROR_INVALID_CONFIG_SPACE`, request consumed), and the
unbind/rebind that used to run on that failure tore down otherwise healthy
sibling routes without ever restoring payload. Live tests across several hosts
never once recovered a data path this way; a true cold power cycle did.

So a route whose data path cannot be proven stays route-local: the frame
session keeps rebuilding it under exponential backoff and logs the dead-path
counters, and a genuinely wedged controller is left for an operator cold power
cycle. The power-cycle mailbox primitive (`dma_port_power_cycle`) remains only
where the core uses it during NVM authentication.

## The probe-time startup one-shot (retained)

Separate from runtime recovery, `icm_root_power_cycle()` runs once during probe
when `DRIVER_READY` or the first root-config read times out. It dispatches a
single power-cycle request over the native DMA-port config path and requires a
fresh probe (both `DRIVER_READY` and root-config replies) before declaring
recovery; repeated failure is terminal. It never unbinds a live domain — it
acts only on a controller that has not yet come up — and it is bounded to one
attempt per PCI function. It has not been observed to recover a wedged
controller on this hardware either, but it is harmless and cheap, so it stays.

## Instrumentation

Distinguish the local firmware completion (PDF `0xc`, `TB_CFG_PKG_ICM_RESP`)
from the peer XDomain response (PDF `0x7`, `TB_CFG_PKG_XDOMAIN_RESP`); the two
are separate state machines and were historically conflated. For a data-path
verdict read the NHI ring producer/consumer indices and the router per-hop
receive counters directly, not the control-plane or hop-enable bits, which stay
healthy while the data engine is dead. `lspci -vv` plus AER/DPC counters cover
the PCIe layer.

Hardware acceptance for a recovered edge requires advancing peer RX counters in
both directions and a five-minute authenticated soak with no ring stall,
AER/DPC growth, unmatched-response escalation, or teardown warning.
