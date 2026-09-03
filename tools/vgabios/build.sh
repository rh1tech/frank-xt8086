#!/bin/sh
# Build the video BIOS that lives at 0xC0000. See README.md for why this
# machine needs one at all, and why it has to be built rather than taken.
#
# Everything happens in a container: the LGPL VGABios needs bcc and as86
# from dev86, which Homebrew does not carry. Nothing is installed on the
# host. The result is copied to roms/vgabios.bin.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
work=${TMPDIR:-/tmp}/frank-vgabios
upstream=https://github.com/qemu/vgabios.git

rm -rf "$work"
git clone --depth 1 "$upstream" "$work"
cp "$here/shortbranch.py" "$work/"

# A plain build: no VBE, no PCI. An XT has no PCI bus, and nothing this
# machine runs asks for VBE. It also keeps the image at exactly 32K, so it
# fits C000-C7FF and leaves the hard disk ROM where it is at C800.
python3 - "$work/Makefile" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
s = s.replace("vgabios.bin              : VGAFLAGS := -DVBE -DPCI_VID=0x1234",
              "vgabios-plain.bin        : VGAFLAGS :=\n"
              "vgabios.bin              : VGAFLAGS := -DVBE -DPCI_VID=0x1234")
s = s.replace("vgabios.bin              : DISTNAME := VGABIOS-lgpl-latest.bin",
              "vgabios-plain.bin        : DISTNAME := VGABIOS-lgpl-plain.bin\n"
              "vgabios.bin              : DISTNAME := VGABIOS-lgpl-latest.bin")
s = s.replace("vgabios.bin              : $(VGA_FILES) $(VBE_FILES) biossums",
              "vgabios-plain.bin        : $(VGA_FILES) biossums\n"
              "vgabios.bin              : $(VGA_FILES) $(VBE_FILES) biossums")
# Lower every conditional branch to something an 8086 can execute.
s = s.replace("\tsed -e 's/^\\.text//' -e 's/^\\.data//' $*.s > _$*_.s\n",
              "\tsed -e 's/^\\.text//' -e 's/^\\.data//' $*.s | python3 shortbranch.py > _$*_.s\n")
open(p, 'w').write(s)
PY

docker run --rm --platform linux/amd64 -v "$work":/src -w /src debian:bookworm sh -c \
  'apt-get update -qq >/dev/null && apt-get install -y -qq bcc gcc make python3 >/dev/null && make vgabios-plain.bin'

cp "$work/VGABIOS-lgpl-plain.bin" "$repo/roms/vgabios.bin"
ls -l "$repo/roms/vgabios.bin"
