# force_sw_cm plug hang on appmana-001 (X570 Creator, Titan Ridge, NVM 45.0)

Status 2026-08-18: instrumented, awaiting one supervised plug test. Do NOT
push/tag v2.37 (8892a0e) until the plug path is proven safe.

## What is established (measured, not inferred)

- v2.36 `force_sw_cm=1` boot at 10:54: probe failed `-ETIMEDOUT` (resident ICM
  serves config space only after DRIVER_READY). v2.37's
  `icm_unlock_config_space()` fixes this: the 11:05:44 boot logged
  "resident ICM opened the config space", the root switch enumerated fully,
  TMU configured, and the reconcile loop re-armed lane detection on ports
  1-4 (lane disable/enable writes to ICM-held ports).
- The machine then ran ~5.5 min stable with the software CM coexisting with
  the resident ICM. **Boot + idle coexistence is proven safe.**
- Plugging the dock froze the whole machine instantly. Not one thunderbolt
  message survived to the journal between the last idle entry (11:11:11,
  unrelated umip spam) and the freeze.
- Recovery was a MANUAL power-cycle (operator statement). The 2m01s gap to
  the next boot is just human + POST time, not a watchdog: at freeze time
  this machine had NO hardware watchdog at all (no /dev/watchdog,
  RuntimeWatchdogSec=off). Nothing bounds a repeat freeze unless one is
  armed first — which has now been done, see below.
- The absence of log output is NOT evidence the driver never ran: journald
  loses the final seconds before a hard freeze (disk flush). The evidence
  channel was missing, so the mechanism (firmware wedge stalling a host MMIO
  read vs. a driver-side hotplug path hang) is still open.

## Directions ruled out

- Halting the resident ICM: forbidden by the firmware RE record in
  `icm_stop()` — on Alpine/Titan Ridge every warm ARC restart skips
  re-authentication (mask-ROM only at true chip reset); poking
  `REG_FW_STS`/CIO reset at runtime converts a broken-but-alive controller
  into a terminally wedged one that only a board power cycle recovers.
- Guess-and-patch on the driver side: the failing access is unknown. The
  fork's own methodology (tbv-hang-repro → capture → tbv-trace-to-kunit)
  requires the capture first; a KUnit contract written now would encode a
  speculation.

## Instrumentation now in place (all boot-persistent, labeled TEMPORARY)

| Piece | Where |
|---|---|
| printk → UDP stream | `netconsole` to appmana-019 10.2.0.57:16666, MAC-pinned, via `/etc/modprobe.d/netconsole-tbdebug.conf` |
| boot-time arming | `netconsole-tbdebug.service` (after network-online; also raises console loglevel and emits a marker) |
| console loglevel 8 | `/etc/sysctl.d/99-tbdebug-printk.conf` — the `thunderbolt.dyndbg=+p` firehose reaches netconsole |
| capture sink | transient unit `netconsole-capture-appmana-001` on appmana-019 → `/var/tmp/appmana-001-netconsole.log` (socat; restart-always, dies if 019 reboots — re-run before testing) |
| hard-lockup naming | NMI watchdog already on (`nmi_watchdog=1`, `hardlockup_panic=0`); a stalled-MMIO core produces "hard LOCKUP" + backtrace on another CPU, which netconsole ships out |
| hardware watchdog | SP5100 TCO, armed 2026-08-18: `/etc/modules-load.d/sp5100-tco-tbdebug.conf` + `/etc/systemd/system.conf.d/90-tbdebug-watchdog.conf` (RuntimeWatchdogSec=2min, systemd petting verified live). NOTE: reset-on-expiry has never fired on this board — treat auto-recovery as hoped-for, not proven |
| armed boot | GRUB entry `Ubuntu (tb force_sw_cm one-shot)` (`/etc/grub.d/40_custom`), selected for exactly one boot with `grub-reboot`; the disarmed entry stays default; `GRUB_RECORDFAIL_TIMEOUT=5` so a watchdog reset never strands the menu |

The end of `icm_unlock_config_space()`'s "opened the config space" message is
the marker that the armed boot took over correctly.

## The plug-test protocol

1. On appmana-019, confirm the sink:
   `systemctl is-active netconsole-capture-appmana-001` (restart via
   `systemd-run --unit=netconsole-capture-appmana-001 --property=Restart=always /usr/bin/socat -u UDP-RECV:16666 "CREATE:/var/tmp/appmana-001-netconsole.log,append"`).
2. On appmana-001: `sudo grub-reboot 'Ubuntu (tb force_sw_cm one-shot)'`,
   then reboot **with the dock unplugged**.
3. After boot, verify from any other host that the 019 log carries the
   netconsole marker and "resident ICM opened the config space".
4. Plug the dock. If the machine freezes, give the newly armed SP5100
   watchdog ~3 minutes to reset the board; if it does not fire (its
   reset-on-expiry is unproven here), power-cycle manually as before.
   Either way the reboot lands on the disarmed default entry.
5. Read `/var/tmp/appmana-001-netconsole.log` on appmana-019. The tail is
   the evidence journald could never keep:
   - thunderbolt dyndbg lines → the software CM's hotplug path ran; the last
     line names the wedging operation.
   - an NMI "hard LOCKUP" backtrace → the exact stalled instruction (MMIO
     read site), naming the wedged access.
   - silence after the plug → the freeze preceded any host-side handling:
     firmware wedged the chip on the physical event itself.
6. Convert the captured sequence into the RED KUnit (cm model in
   `drivers/thunderbolt/test.c`, mock-firmware style) and only then write
   the v2.38 mitigation.

## RESOLVED 2026-08-20: the capture named the mechanism

The instrumented plug test captured the whole sequence on appmana-019:

1. t=228.2: the software CM synthesized the (correctly) lost plug, scanned,
   and enumerated the Razer Thunderbolt 4 Dock (Goshen Ridge, 20 Gb/s dual
   lane) at route 0x1; PCIe Down/Up paths activated. **The plug worked.**
2. t=228.2: CLx CL0s/CL1 was enabled on the dock link (no `clx=0` on the
   armed boot).
3. t=242.6: the resident ICM emitted its stray hot event
   ("unexpected event 0xa, ignoring" — correctly dropped at packet level).
4. t=244.6: the reconcile poll read the root port lane state as
   UNPLUGGED(7) **while the dock still answered config space** — the
   warning itself printed "router present but lane state 7" — and
   synthesized an unplug anyway: tore down the live PCIe tunnel, removed
   the device, and kicked the PHY (lane disable/enable) of a live link.
5. t=247.0: root-switch config reads began timing out (the resident ICM
   chewing on the real link edge the kick fed it), then the domain went
   silent forever. Kernel alive (mouse moving), TB domain dead, desktop
   D-stated behind its sysfs. Manual reboot.

So the lethal defect was OURS: the reconcile heuristic trusted one
lane-state register sample over the reachability it had already
demonstrated, on a register a coexisting master (and CLx) can perturb.

## The v2.38 fix (KUnit red-first: tb_test_cm_reconcile_perturbed_lane_keeps_live_link)

- `tb_reconcile_peer_reachable()`: a bare UNPLUGGED sample is confirmed
  with a bounded (500 ms, 1 dword) config read of the enumerated
  child/XDomain router before any unplug is synthesized. A dead peer
  (the appmana-008/019 stranding this reconcile exists for) fails the
  probe and still converges; a live link with a lying register is left
  alone. Condemned XDomains (`is_unplugged`) tear down on that signal
  as before.
- CLx is refused entirely under `force_sw_cm` (and `thunderbolt.clx=0`
  added to the one-shot GRUB entry as belt) — fleet-consistent, and it
  removes the state-register ambiguity at the source.
- The reconcile pass ends early on the first failed lane-state read
  instead of burning a timeout per port while holding `tb->lock` — that
  lock-hold is what starves sysfs readers into D state and freezes the
  desktop when the control channel goes sick.

## Superseded hypotheses (kept for the record)

- H1 (firmware-first): the resident ICM's broken hot-plug path wedges the
  Titan Ridge on the physical connect (its lane/port state disagrees with
  the lane re-arms the software CM performed); the next host MMIO to the
  NHI (ring0 MSI-X handler) stalls the CPU. Capture signature: little or no
  driver activity, then a hard-LOCKUP backtrace inside `nhi_msi`/`ring_msix`
  or total silence.
- H2 (driver-first): the software CM's hotplug/scan path performs an access
  (config read/write, lane or TMU write) the coexisting firmware cannot
  tolerate, wedging the chip mid-sequence. Capture signature: the dyndbg
  hotplug trail up to a specific operation, then lockup/silence.
- Either way the lethal writes candidates are already narrowed: the only
  ICM-owned state the software CM mutates while idle are the lane
  enable/disable re-arms and TMU mode; on plug it would additionally
  configure the new router and enable tunnels.

## 2026-08-20 second instrumented plug (v2.38): TB layer healthy, machine is hotplug-deaf

With the probe-gated reconcile + clx=0, the plug enumerated the dock, the
PCIe tunnel activated, and the link stayed up past the old death window —
no false unplug, no hang. "Nothing happened" was the NEXT layer down:

- 05:01.0 (TR tunnel bridge): LnkSta 2.5GT/s x4, PresDet+ — the dock's
  PCIe upstream trained. Resources are ample (64M MMIO + 64M prefetch +
  51 buses per tunnel port; only the 16K I/O windows failed, which
  MMIO-only dock devices do not need).
- But SltCtl has every hotplug interrupt disabled and /sys/bus/pci/slots
  is empty: pciehp owns nothing, because
  `_OSC: not requesting OS control; OS requires [ExtendedConfig ASPM
  ClockPM MSI]` — the long-standing `pcie_aspm=off` on the cmdline fails
  the _OSC preconditions, so the kernel never requested native PCIe
  hotplug and the platform firmware (the broken TBT stack) retained it.
  This machine has been hotplug-deaf for tunnel devices all along,
  under the ICM firmware CM too.

Accommodation: `pcie_ports=native` added to the one-shot GRUB entry —
native hotplug/PME control regardless of _OSC, keeping pcie_aspm=off.
Expected on the next armed boot: pciehp binds 05:01.0/05:04.0, and the
tunnel link-up raises a hotplug interrupt that enumerates the dock.
