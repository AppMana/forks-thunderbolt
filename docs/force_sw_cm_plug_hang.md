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
- The next boot started 11:13:12 — 2m01s after the last journal entry. That
  is the SP5100 TCO watchdog timeout (systemd pets it with a 2 min budget).
  **The "hard hang" was a genuine full-CPU freeze terminated by the hardware
  watchdog**: systemd stopped petting, the board auto-reset. Worst case of a
  repeat is a ~2-minute outage plus auto-reboot.
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
4. Plug the dock. If the machine freezes, wait out the 2-minute watchdog;
   it reboots into the disarmed default entry on its own.
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

## Open hypotheses the capture will separate

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
