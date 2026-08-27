#!/usr/bin/env python3
"""Dump the NHI ring producer/consumer registers and ring options for a hop.

This reads BAR0 of the NHI PCI function directly, which is the only way to see
what the *hardware* thinks the ring state is. The driver's own head/tail are
software shadows; when a TX ring stops completing, the question is whether the
NHI's consumer index is still advancing, and that lives only in this register.

Register layout, from drivers/thunderbolt/nhi_regs.h and the accessors in
nhi.c (ring_iowrite_prod / ring_iowrite_cons):

  TX ring desc base  = 0x00000 + hop*16
  RX ring desc base  = 0x08000 + hop*16
    +0x00  u64  physical pointer to the descriptor array
    +0x08  u32  packed producer/consumer:
                 TX -> bits 31:16 driver head (prod), bits 15:0 NHI tail
                 RX -> bits 15:0  driver head,        bits 31:16 NHI tail
    +0x0c  u32  descriptor count (and, for RX, max frame size)

  TX options base    = 0x19800 + hop*32
  RX options base    = 0x29800 + hop*32
    +0x00  u32  enum ring_flags; bit 31 = RING_FLAG_ENABLE

Usage: nhi-ring-regs.py <pci-bdf> <hop> [more hops...]
   eg: sudo nhi-ring-regs.py 0000:06:00.0 3
"""
import mmap
import os
import struct
import sys

REG_TX_RING_BASE = 0x00000
REG_RX_RING_BASE = 0x08000
REG_TX_OPTIONS_BASE = 0x19800
REG_RX_OPTIONS_BASE = 0x29800
REG_CAPS = 0x39640

RING_FLAG_ISOCH_ENABLE = 1 << 27
RING_FLAG_E2E_FLOW_CONTROL = 1 << 28
RING_FLAG_PCI_NO_SNOOP = 1 << 29
RING_FLAG_RAW = 1 << 30
RING_FLAG_ENABLE = 1 << 31


def ring_distance(producer, consumer, count):
    """Return a ring distance using the ring's configured wrap point."""
    if count <= 0:
        raise ValueError("ring count must be positive")
    return (producer - consumer) % count


def decode_flags(v):
    names = []
    for bit, name in (
        (RING_FLAG_ENABLE, "ENABLE"),
        (RING_FLAG_RAW, "RAW"),
        (RING_FLAG_PCI_NO_SNOOP, "NO_SNOOP"),
        (RING_FLAG_E2E_FLOW_CONTROL, "E2E"),
        (RING_FLAG_ISOCH_ENABLE, "ISOCH"),
    ):
        if v & bit:
            names.append(name)
    return "|".join(names) if names else "none"


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    bdf = sys.argv[1]
    hops = [int(h) for h in sys.argv[2:]]
    path = f"/sys/bus/pci/devices/{bdf}/resource0"
    size = os.path.getsize(path)
    fd = os.open(path, os.O_RDONLY | os.O_SYNC)
    try:
        mm = mmap.mmap(fd, size, mmap.MAP_SHARED, mmap.PROT_READ)
    finally:
        os.close(fd)

    def u32(off):
        return struct.unpack_from("<I", mm, off)[0]

    def u64(off):
        return struct.unpack_from("<Q", mm, off)[0]

    print(f"NHI {bdf} bar0 size=0x{size:x}")
    caps = u32(REG_CAPS)
    hop_count = caps & 0x3FF
    if caps == 0xFFFFFFFF or not hop_count:
        sys.exit(f"NHI {bdf}: invalid capabilities register 0x{caps:08x}")
    print(f"capabilities=0x{caps:08x} hop_count={hop_count}")
    for hop in hops:
        if hop < 0 or hop >= hop_count:
            print(f"\n== hop {hop}: outside controller range 0..{hop_count - 1}")
            continue
        tx = REG_TX_RING_BASE + hop * 16
        rx = REG_RX_RING_BASE + hop * 16
        txo = u32(REG_TX_OPTIONS_BASE + hop * 32)
        rxo = u32(REG_RX_OPTIONS_BASE + hop * 32)

        tx_pc = u32(tx + 8)
        tx_prod = (tx_pc >> 16) & 0xFFFF
        tx_cons = tx_pc & 0xFFFF
        tx_count_reg = u32(tx + 12)
        tx_count = tx_count_reg & 0xFFFF
        rx_pc = u32(rx + 8)
        rx_cons = rx_pc & 0xFFFF
        rx_prod = (rx_pc >> 16) & 0xFFFF
        rx_count_reg = u32(rx + 12)
        rx_count = rx_count_reg & 0xFFFF

        print(f"\n== hop {hop}")
        print(f"  TX desc_phys=0x{u64(tx):016x} count_reg=0x{tx_count_reg:08x}")
        if tx_count:
            print(f"  TX prod/cons raw=0x{tx_pc:08x} driver_prod={tx_prod} "
                  f"nhi_cons={tx_cons} queued={ring_distance(tx_prod, tx_cons, tx_count)}")
        else:
            print(f"  TX prod/cons raw=0x{tx_pc:08x} driver_prod={tx_prod} "
                  f"nhi_cons={tx_cons} queued=unconfigured")
        print(f"  TX options=0x{txo:08x} [{decode_flags(txo)}]")
        print(f"  RX desc_phys=0x{u64(rx):016x} count_reg=0x{rx_count_reg:08x}")
        if rx_count:
            print(f"  RX cons/prod raw=0x{rx_pc:08x} driver_cons={rx_cons} "
                  f"nhi_prod={rx_prod} armed={ring_distance(rx_cons, rx_prod, rx_count)}")
        else:
            print(f"  RX cons/prod raw=0x{rx_pc:08x} driver_cons={rx_cons} "
                  f"nhi_prod={rx_prod} armed=unconfigured")
        print(f"  RX options=0x{rxo:08x} [{decode_flags(rxo)}]")
    mm.close()


if __name__ == "__main__":
    main()
