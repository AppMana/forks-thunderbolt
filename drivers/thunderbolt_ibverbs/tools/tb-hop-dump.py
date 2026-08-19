#!/usr/bin/env python3
"""Decode router hop entries from thunderbolt debugfs path files.

Reads /sys/kernel/debug/thunderbolt/<router>/port*/path and decodes every
(or a filtered set of) hop entries per struct tb_regs_hop, so the
capture-before-fix protocol for the one-way fabric wedge can prove whether
the session's hop entries are programmed/enabled (credit-state corruption)
or missing/half-programmed (entry corruption) while a TX ring shows posted
descriptors with zero consumption.

Usage:
  sudo tb-hop-dump.py                       # every enabled entry, all routers
  sudo tb-hop-dump.py --all                 # include disabled entries
  sudo tb-hop-dump.py --hopid 8 --hopid 9   # only these in-HopIDs
  sudo tb-hop-dump.py --router 0-0          # one router directory

Pair with tools/nhi-ring-regs.py (ring enable/prod/cons) and the per-port
debugfs `counters` files for a full wedge snapshot.
"""

import argparse
import glob
import os
import sys

DEBUGFS = "/sys/kernel/debug/thunderbolt"


def decode(hopid, d0, d1):
    return {
        "in_hopid": hopid,
        "next_hop": d0 & 0x7FF,
        "out_port": (d0 >> 11) & 0x3F,
        "initial_credits": (d0 >> 17) & 0x7F,
        "pmps": (d0 >> 24) & 1,
        "enable": (d0 >> 31) & 1,
        "weight": d1 & 0xF,
        "priority": (d1 >> 8) & 0x7,
        "drop": (d1 >> 11) & 1,
        "counter": (d1 >> 12) & 0x7FF,
        "counter_en": (d1 >> 23) & 1,
        "ingress_fc": (d1 >> 24) & 1,
        "egress_fc": (d1 >> 25) & 1,
        "ingress_sb": (d1 >> 26) & 1,
        "egress_sb": (d1 >> 27) & 1,
        "pending": (d1 >> 28) & 1,
        "raw": (d0, d1),
    }


def parse_path_file(path):
    """Yield (hopid, dword0, dword1) tuples from one debugfs path file."""
    entries = {}
    with open(path) as f:
        for line in f:
            if line.startswith("#") or "<not accessible>" in line:
                continue
            parts = line.split()
            if len(parts) != 4:
                continue
            rel = int(parts[1])
            hopid = int(parts[2], 16)
            val = int(parts[3], 16)
            entries.setdefault(hopid, [None, None])[rel % 2] = val
    for hopid in sorted(entries):
        d0, d1 = entries[hopid]
        if d0 is None or d1 is None:
            continue
        yield hopid, d0, d1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--all", action="store_true",
                    help="include disabled hop entries")
    ap.add_argument("--hopid", type=int, action="append", default=[],
                    help="only these in-HopIDs (repeatable)")
    ap.add_argument("--router", default=None,
                    help="only this router directory name (e.g. 0-0)")
    args = ap.parse_args()

    if not os.path.isdir(DEBUGFS):
        sys.exit(f"{DEBUGFS} not present (CONFIG_USB4_DEBUGFS / mount?)")

    pattern = os.path.join(DEBUGFS, args.router or "*", "port*", "path")
    files = sorted(glob.glob(pattern))
    if not files:
        sys.exit(f"no path files match {pattern}")

    for fn in files:
        router = fn.split(os.sep)[-3]
        port = fn.split(os.sep)[-2]
        try:
            rows = list(parse_path_file(fn))
        except OSError as e:
            print(f"{router}/{port}: unreadable ({e})")
            continue
        shown = False
        for hopid, d0, d1 in rows:
            e = decode(hopid, d0, d1)
            if args.hopid and hopid not in args.hopid:
                continue
            if not e["enable"] and not args.all:
                continue
            if not shown:
                print(f"== {router}/{port}")
                shown = True
            flags = "".join([
                "E" if e["enable"] else "-",
                "P" if e["pending"] else "-",
                "i" if e["ingress_fc"] else "-",
                "e" if e["egress_fc"] else "-",
                "d" if e["drop"] else "-",
            ])
            print(f"  in_hop {hopid:4d} -> out_port {e['out_port']:2d} "
                  f"next_hop {e['next_hop']:4d} credits {e['initial_credits']:3d} "
                  f"prio {e['priority']} weight {e['weight']:2d} [{flags}] "
                  f"raw {e['raw'][0]:#010x} {e['raw'][1]:#010x}")


if __name__ == "__main__":
    main()
