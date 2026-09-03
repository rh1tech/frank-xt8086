# The video BIOS

`roms/vgabios.bin` is the LGPL Plex86/Bochs VGABios, built by `build.sh`
with neither VBE nor PCI, and with every conditional branch lowered to
something an 8086 can execute. `build.sh` reproduces it exactly.

## Why this machine needs one

GLaBIOS's INT 10h is CGA and MDA only. Its own source sizes memory as
"640K base 0000-A000 for EGA/VGA" against 736K for CGA, and the only
video option it takes at build time is CGA/MDA light pen support. What it
does for an EGA or a VGA is what a real PC BIOS does: detect one during
POST and stand aside, expecting the card to bring its own BIOS in the
C000 option ROM scan.

Without that ROM, every INT 10h call software makes in a planar mode --
write a character, scroll a window, read the cell under the cursor -- is
answered by code written for a CGA. The firmware can set the modes
through its own path, and does, but that is a mode setter and not a video
BIOS.

## Why it has to be built

Two candidates do not work here.

**SeaVGABIOS**, which murm386 pairs with its card, is built by SeaBIOS
with `-m32 -march=i386` and `-m16`. It needs a 386. On an 8086 or a V20
its init runs a few instructions and falls into unmapped memory.

**The LGPL VGABios as upstream builds it** is compiled by bcc, which does
target an 8086, but assembled by as86, whose `-0` selects 16-bit output
rather than an 8086 instruction set. Any conditional branch landing
outside +/-127 bytes comes out as `0F 8x`, the 386 long form -- 72 of
them in a plain build. On an 8086 and on a V20, `0F` is `POP CS`: it pops
a word into CS and the machine leaves for somewhere else. Traced on
hardware, the first one reached was at C000:7C6B, and the next thing the
CPU did was read the stack and jump to segment zero.

`shortbranch.py` rewrites each conditional branch as an inverted short
branch over a near jump:

    beq  target      ->    jne  .q<n>
                           jmp  target
                           .q<n>:

The inverted branch now spans three bytes so it always fits the short
form, and the unconditional jump is 8086 code at any distance. That takes
the count of 386 instructions in the image to zero, and costs about 350
bytes -- the image still ends at 0x7E8D, inside its 32K.

## Provenance

Upstream is https://github.com/qemu/vgabios, LGPL 2.1, and `build.sh`
clones it at build time rather than vendoring it. The only local changes
are the plain build target and the branch lowering above, both applied by
`build.sh` so they can be read and re-applied.
