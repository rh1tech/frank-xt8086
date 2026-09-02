#!/usr/bin/env python3
"""
frank-xt8086 — an RP2350B acting as the whole chipset for a real 8086

Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
SPDX-License-Identifier: GPL-3.0-or-later

Splice tools/rom/mbr.asm into a disk image's sector 0, leaving the
partition table and everything after it alone.

Only the first 446 bytes are touched. Rewriting the whole sector would
mean rebuilding the partition table too, and a partition table is the one
thing on a disk that is genuinely expensive to get wrong.
"""
import subprocess, sys, pathlib

if len(sys.argv) != 2:
    sys.exit("usage: build_mbr.py <image>")

here = pathlib.Path(__file__).parent
img  = pathlib.Path(sys.argv[1])
tmp  = here / "mbr.bin"

subprocess.run(["nasm", "-f", "bin", str(here / "mbr.asm"), "-o", str(tmp)], check=True)
code = tmp.read_bytes()
tmp.unlink()

if len(code) != 446:
    sys.exit(f"MBR code is {len(code)} bytes; it must be exactly 446")

d = bytearray(img.read_bytes())
if d[510:512] != b"\x55\xAA":
    sys.exit("image has no boot signature; partition it first")

d[0:446] = code
img.write_bytes(bytes(d))

active = [i for i in range(4) if d[446 + i*16] == 0x80]
print(f"{img.name}: MBR installed, "
      f"{'partition %d active' % active[0] if active else 'no active partition (boots the floppy)'}")
