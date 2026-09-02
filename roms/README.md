# ROM images

These are build inputs, not build products: `IMPORT_BIN` in
`src/core/state.c` welds them into the firmware binary with `.incbin`, so
the repository does not build without them. That is why they are tracked
despite the `*.bin` rule in `.gitignore`.

None of them are ours. All three are redistributable, and each prints its
own licence on the boot screen.

| File | What | Author | Licence |
|------|------|--------|---------|
| `GLABIOS.ROM` | GLaBIOS, the 8 KB system BIOS at `0xFE000` | 640KB | GPL-3.0 |
| `ide_xt.bin` | XTIDE Universal BIOS, the hard-disk option ROM at `0xC8000` | XTIDE project | GPL-2.0 |
| `os.img` | bootOS, a 512-byte operating system in one boot sector | Óscar Toledo G. | MIT |

`os.img` is the fallback boot medium: drive A: falls back to it when no
floppy image is open, so a board with no microSD — or one with nothing
selected in SETUP — still reaches something you can type at rather than
sitting at "Boot sector not found". The normal boot media are `.img`
files on the card, chosen in SETUP; see `src/ui/setup_menu.c`.

## Replacing one

`GLABIOS.ROM` must be exactly 8 KB and is mapped at `0xFE000`, the top of
the 8086's address space, because that is where the CPU's reset vector
sends it. `BIOS_ROM_SIZE` and `BIOS_ROM_BASE` in `src/core/xt8086.h` are
the two constants to change together if you want a larger one.

`src/core/state.c` has commented-out `IMPORT_BIN` lines for two
alternatives that were useful during bring-up — Ruud's diagnostic ROM and
the Landmark test ROM. Neither is included here; drop them in this
directory and swap the line if you need them.

Upstream:

- GLaBIOS — <https://github.com/640-KB/GLaBIOS>
- XTIDE Universal BIOS — <https://www.xtideuniversalbios.org/>
- bootOS — <https://github.com/nanochess/bootOS>
