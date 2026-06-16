#!/usr/bin/env bash
# Recover a wedged Thunderbolt NHI without a full reboot.
#
# After a thunderbolt_ibverbs data-path stall the NHI can refuse config
# reads ("failed to read root switch config space", probe error -110) and
# a warm reboot does NOT clear it on Maple Ridge / Titan Ridge discrete
# controllers. Cheapest-first recovery ladder:
#   1) PCI secondary-bus reset of the NHI (reset_method=bus) -- seconds.
#   2) D3cold via runtime PM if the platform wired _PR3 -- cold-starts the
#      controller silicon without a host reboot.
#
# Usage: tbv-nhi-reset.sh [--d3cold]
set -euo pipefail

D3COLD=0
[ "${1:-}" = "--d3cold" ] && D3COLD=1

NHI=$(lspci -D | awk '/Thunderbolt.*NHI/{print $1; exit}')
[ -n "$NHI" ] || { echo "no Thunderbolt NHI found"; exit 1; }
DEV=/sys/bus/pci/devices/$NHI
echo "NHI=$NHI state=$(cat "$DEV/power_state" 2>/dev/null) reset_methods=$(cat "$DEV/reset_method" 2>/dev/null)"

if [ -e "$DEV/driver" ]; then
  echo "unbinding $(basename "$(readlink "$DEV/driver")")"
  echo "$NHI" | sudo tee "$DEV/driver/unbind" >/dev/null || true
  sleep 1
fi

if [ "$D3COLD" = 1 ]; then
  echo "attempting D3cold via runtime PM"
  echo auto | sudo tee "$DEV/power/control" >/dev/null
  echo 0 | sudo tee "$DEV/power/autosuspend_delay_ms" >/dev/null 2>&1 || true
  sleep 3
  echo "power_state=$(cat "$DEV/power_state") real_power_state=$(cat "$DEV/firmware_node/real_power_state" 2>/dev/null || echo n/a)"
  echo on | sudo tee "$DEV/power/control" >/dev/null
  sleep 2
else
  echo "issuing secondary-bus reset"
  echo 1 | sudo tee "$DEV/reset" >/dev/null
  sleep 2
fi

echo "$NHI" | sudo tee /sys/bus/pci/drivers/thunderbolt/bind >/dev/null 2>&1 || true
sleep 4

if ls /sys/bus/thunderbolt/devices/ 2>/dev/null | grep -q '^0-'; then
  echo "RECOVERED: thunderbolt domain re-enumerated"
  ls /sys/bus/thunderbolt/devices/ | grep '^0-' | head
else
  echo "NOT RECOVERED via $([ "$D3COLD" = 1 ] && echo D3cold || echo bus-reset)."
  [ "$D3COLD" = 0 ] && echo "  retry with --d3cold, else cold power cycle the node."
  exit 2
fi
