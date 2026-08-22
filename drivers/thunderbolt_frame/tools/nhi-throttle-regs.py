#!/usr/bin/env python3
"""Dump NHI per-vector interrupt throttling registers (REG_INT_THROTTLING_RATE
0x38c00 + vector*4, units of 256 ns) from BAR0. Usage: nhi-throttle.py <bdf>"""
import mmap, os, struct, sys

bdf = sys.argv[1]
path = f"/sys/bus/pci/devices/{bdf}/resource0"
fd = os.open(path, os.O_RDONLY)
size = os.fstat(fd).st_size
mm = mmap.mmap(fd, size, prot=mmap.PROT_READ)
for v in range(16):
    off = 0x38C00 + v * 4
    (val,) = struct.unpack_from("<I", mm, off)
    interval = val & 0xFFFF
    print(f"vector {v:2d}: raw=0x{val:08x} interval={interval} ({interval * 256} ns)")
mm.close()
os.close(fd)
