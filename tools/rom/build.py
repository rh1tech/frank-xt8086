#!/usr/bin/env python3
"""
frank-xt8086 — an RP2350B acting as the whole chipset for a real 8086

Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
https://github.com/rh1tech/frank-xt8086
SPDX-License-Identifier: GPL-3.0-or-later

Assemble the option ROM and make it something a BIOS will accept.

Two things the assembler cannot do for itself:

  - the image must be a whole number of 512-byte blocks, and exactly the
    number the third byte of the header claims;
  - the bytes of that image must sum to zero modulo 256, which is the
    only check the BIOS makes before it far-calls into us.

Get either wrong and GLaBIOS silently skips the ROM. There is no error
message to chase, so both are asserted here rather than assumed.
"""
import subprocess, sys, pathlib

here = pathlib.Path(__file__).parent
src  = here / "xtrom.asm"
out  = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else here / "xtrom.bin"

raw = here / "xtrom.raw"
subprocess.run(["nasm", "-f", "bin", str(src), "-o", str(raw)], check=True)
img = bytearray(raw.read_bytes())
raw.unlink()

blocks = img[2]
size   = blocks * 512
if len(img) > size - 1:
    sys.exit(f"ROM is {len(img)} bytes but the header claims {size}; "
             f"raise ROM_BLOCKS in {src.name}")

img += bytes(size - len(img))

# The checksum byte goes last. Sum everything else, then store whatever
# makes the total come out at zero.
img[size - 1] = (-sum(img[:size - 1])) & 0xFF
assert sum(img) % 256 == 0

out.write_bytes(bytes(img))
print(f"{out.name}: {size} bytes, {blocks} blocks, checksum ok")
