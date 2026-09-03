/*
 * frank-xt8086 — an RP2350B acting as the whole chipset for a real 8086
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "vgacard.h"

#include <stdio.h>
#include <string.h>

#include <hardware/sync.h>
#include <pico/time.h>

#include "vga.h"
#include "vgamodes.h"

/*
 * The card's memory is the machine's one video buffer.
 *
 * Four 64K planes interleaved, which is what a VGA has and what
 * murm386's vga.c indexes. It is the same array the CGA, Tandy and
 * Hercules paths use, because only one adapter exists at a time.
 */
extern uint8_t video_memory[];   // core/state.c
#define vga_ram (video_memory + VGACARD_RAM_BASE)

static VGAState *vga;

/*
 * vga.c wants a microsecond clock for cursor blink and retrace timing.
 * It is the only thing it asks of the host besides memory.
 */
uint32_t get_uticks(void) { return (uint32_t)time_us_64(); }

void vgacard_init(void) {
    memset(vga_ram, 0, VGACARD_RAM_SIZE);
    // No framebuffer: this machine draws from the card's memory a
    // scanline at a time, so the width and height given here only have
    // to be something vga.c will accept.
    vga = vga_init((char *)vga_ram, (int)VGACARD_RAM_SIZE, NULL, 640, 480);
}

/*
 * Play a mode's register table into the card.
 *
 * Exactly the sequence a video BIOS would have written, in the order the
 * hardware requires: misc output, sequencer, CRTC (with the write-protect
 * bit cleared first, or the first eight CRTC registers are ignored),
 * graphics controller, then the attribute controller -- whose flip-flop
 * has to be reset by reading the status register before it will take an
 * index.
 */
bool vgacard_set_bios_mode(const uint8_t mode) {
    if (!vga) return false;
    const VgaMode *m = vgamodes_find(mode);
    if (!m) return false;

    const VgaRegs *r = m->regs;
    const uint16_t crtc = m->crtc_base;

    vga_ioport_write(vga, 0x3C2, r->misc);

    for (uint8_t i = 0; i < 5; i++) {
        vga_ioport_write(vga, 0x3C4, i);
        vga_ioport_write(vga, 0x3C5, r->seq[i]);
    }

    vga_ioport_write(vga, crtc, 0x11);
    vga_ioport_write(vga, crtc + 1, r->crtc[0x11] & 0x7F);

    for (uint8_t i = 0; i < 25; i++) {
        vga_ioport_write(vga, crtc, i);
        vga_ioport_write(vga, crtc + 1, r->crtc[i]);
    }

    for (uint8_t i = 0; i < 9; i++) {
        vga_ioport_write(vga, 0x3CE, i);
        vga_ioport_write(vga, 0x3CF, r->gc[i]);
    }

    (void)vga_ioport_read(vga, crtc + 6);       // reset the AC flip-flop

    for (uint8_t i = 0; i < 21; i++) {
        vga_ioport_write(vga, 0x3C0, i);
        vga_ioport_write(vga, 0x3C0, r->ac[i]);
    }
    vga_ioport_write(vga, 0x3C0, 0x20);         // re-enable video

    /*
     * Load the default palette into the DAC.
     *
     * The attribute registers do not hold colours, they hold DAC indices,
     * so a card whose DAC is empty shows the right picture in black. A
     * real video BIOS writes this table at every mode set; ours has to as
     * well.
     *
     * The first 64 entries are the EGA colour space. Its six bits are
     * r g b R G B -- a primary bit and a secondary bit for each channel,
     * the secondary worth half -- which gives four levels a channel,
     * spread across the DAC's 0..63.
     */
    for (unsigned i = 0; i < 64; i++) {
        const unsigned r = (((i >> 2) & 1) << 1) | ((i >> 5) & 1);
        const unsigned g = (((i >> 1) & 1) << 1) | ((i >> 4) & 1);
        const unsigned b = (((i >> 0) & 1) << 1) | ((i >> 3) & 1);
        vga_ioport_write(vga, 0x3C8, i);
        vga_ioport_write(vga, 0x3C9, r * 21);
        vga_ioport_write(vga, 0x3C9, g * 21);
        vga_ioport_write(vga, 0x3C9, b * 21);
    }

    // Mode 13h wants all 256: greys above the colours, then a cube.
    if (mode == 0x13) {
        for (unsigned i = 64; i < 256; i++) {
            unsigned r, g, b;
            if (i < 80) { r = g = b = (i - 64) * 4; }
            else {
                const unsigned j = i - 80;
                r = (j / 36) % 6 * 12; g = (j / 6) % 6 * 12; b = j % 6 * 12;
            }
            vga_ioport_write(vga, 0x3C8, i);
            vga_ioport_write(vga, 0x3C9, r);
            vga_ioport_write(vga, 0x3C9, g);
            vga_ioport_write(vga, 0x3C9, b);
        }
    }

    // A mode set leaves a blank screen; the card's memory is ours to clear.
    memset(vga_ram, 0, VGACARD_RAM_SIZE);

    int w = 0, h = 0;
    printf("[vga] mode %02X -> submode %d %dx%d\n",
           mode, vga_get_graphics_mode(vga, &w, &h), w, h);
    return true;
}

void vgacard_port_write(const uint16_t port, const uint8_t value) {
    if (!vga) return;


    vga_ioport_write(vga, port, value);
}

uint8_t vgacard_port_read(const uint16_t port) {
    return vga ? (uint8_t)vga_ioport_read(vga, port) : 0xFFu;
}


void vgacard_mem_write(const uint32_t addr, const uint8_t value) {
    if (vga) vga_mem_write(vga, addr, value);
}

uint8_t vgacard_mem_read(const uint32_t addr) {
    return vga ? vga_mem_read(vga, addr) : 0xFFu;
}

const uint32_t *vgacard_planes(void) { return (const uint32_t *)vga_ram; }

// See vgacard.h: real VGA CRTCs wrap text addressing at 14 bits (8192
// cells) regardless of how much VRAM the card has. TEXT_ROW_MARGIN cells
// of the start are mirrored past the end so a row that starts near the
// wrap and is read one cell at a time, uncorrected mid-row, lands on the
// correctly-wrapped cell instead of running off this buffer -- 256 is
// comfortably past the widest row vgacard_text_geometry() will hand out
// (132 columns).
#define TEXT_CELL_COUNT (VGACARD_TEXT_CELL_MASK + 1u)
#define TEXT_ROW_MARGIN 256u
static uint32_t text_snapshot[TEXT_CELL_COUNT + TEXT_ROW_MARGIN];

static vgacard_text_t pending_text;
static bool           pending_text_valid;

/*
 * Making vgacard_snap_text()'s commit into mc6845 a single atomic step
 * (see vgacard.h) fixed only half of this race. The other half is here:
 * pending_text is a multi-field struct, main.c's writer runs on core 0's
 * main loop, and the scanline interrupt that reads it runs on core 0 too,
 * at higher priority, free to preempt the write between any two of its
 * field copies -- the exact same hazard the mc6845 fields had, one level
 * further back. Both sides are a handful of bytes; disabling interrupts
 * around each is not a real cost, and closing this half of the race
 * costs nothing even if the other half turns out to have been the whole
 * story.
 */
void vgacard_stage_text_geometry(const vgacard_text_t *t) {
    const uint32_t irq_state = save_and_disable_interrupts();
    pending_text = *t;
    pending_text_valid = true;
    restore_interrupts(irq_state);
}

// vgacard is deliberately isolated from core/ (see this file's
// CMakeLists.txt), so the actual mc6845 commit happens in
// drivers/vga/vga.c, which already reaches it; this just hands over
// what to commit.
bool vgacard_pending_text_geometry(vgacard_text_t *out) {
    const uint32_t irq_state = save_and_disable_interrupts();
    const bool valid = pending_text_valid;
    if (valid) *out = pending_text;
    restore_interrupts(irq_state);
    return valid;
}

/*
 * Copying the whole 8192-cell window unconditionally cost more than it
 * needed to on the common case (a screen using only its first couple of
 * thousand cells), and copying at all does not prove anything: a single
 * memcpy() has no way to tell a clean read from one that landed
 * mid-write, and the guest can be writing anywhere in this window at any
 * time -- not just during a mode transition, since nothing here is
 * exclusive to the guest's own screen output.
 *
 * Two fixes, not one: shrink the copy to what this frame actually needs
 * (cutting the odds of the guest's own writes landing inside the window
 * at all), and verify it by re-checksumming the live memory right after
 * copying -- if the checksum moved, the guest wrote during the copy and
 * the copy is retried. RAM here is too tight for a second full-size
 * buffer to compare against directly, which is the only reason this is
 * a checksum and not a byte-for-byte re-check.
 */
void vgacard_snap_text(void) {
    if (!vga) return;

    uint32_t cells_needed = TEXT_CELL_COUNT;
    if (pending_text_valid) {
        const uint32_t extent = (uint32_t)pending_text.start_addr
                               + (uint32_t)pending_text.columns * pending_text.rows;
        if (extent < TEXT_CELL_COUNT) cells_needed = extent;
    }

    const uint32_t *src = (const uint32_t *)vga_ram;

    for (int attempt = 0; attempt < 3; attempt++) {
        uint32_t sum1 = 0;
        for (uint32_t i = 0; i < cells_needed; i++) {
            const uint32_t v = src[i];
            text_snapshot[i] = v;
            sum1 += v ^ (i * 2654435761u); // position-mixed: a shifted
                                            // write, not just a changed
                                            // one, still moves this
        }
        uint32_t sum2 = 0;
        for (uint32_t i = 0; i < cells_needed; i++) {
            sum2 += src[i] ^ (i * 2654435761u);
        }
        if (sum1 == sum2) break; // unchanged across the copy: not torn
        // Guest wrote somewhere in here while this ran -- try again.
    }

    memcpy(text_snapshot + TEXT_CELL_COUNT, vga_ram, TEXT_ROW_MARGIN * sizeof(uint32_t));
}

const uint32_t *vgacard_text_planes(void) { return text_snapshot; }

void vgacard_snap_frame(void) {
    if (!vga) return;

    const uint8_t *cr = vga->cr;

    // Bounded: this runs in the scanline interrupt, and a guest writing
    // the pair continuously must not be able to hold it there.
    uint8_t hi = cr[0x0C], lo = cr[0x0D];
    for (int tries = 0; tries < 4 && hi != cr[0x0C]; tries++) {
        hi = cr[0x0C];
        lo = cr[0x0D];
    }
    vga_frame.start_addr = (uint16_t)((uint16_t)hi << 8 | lo);

    vga_frame.panning = (uint8_t)(vga->ar[0x13] & 0x07u);

    const int lc = (int)cr[0x18]
                 | (((int)cr[0x07] & 0x10) << 4)
                 | (((int)cr[0x09] & 0x40) << 3);
    vga_frame.line_compare = lc;
}

bool vgacard_text_geometry(vgacard_text_t *out) {
    if (!vga) return false;

    /*
     * gr[0x06] bit 0 is the card's own answer to "text or graphics" --
     * vga_refresh() in this same driver already keys off exactly this
     * bit to pick graphic_mode. Without checking it here too, a CRTC
     * programmed for a plausible-looking column/row count while the
     * card is actually in graphics mode (as it briefly is during the
     * option ROM's own POST, right after its banner, while it probes
     * modes) was read as text: the character+attribute pairs this
     * function's caller expects were live planar pixel data instead,
     * rendered through the text path as whatever garbage that pixel
     * data happened to decode to as glyphs and colours. On real
     * hardware this window is too short to see; polled once a frame
     * from a real 8086 slow enough to keep the option ROM busy for
     * several frames, it was not.
     */
    if (vga->gr[0x06] & 1) return false;

    const uint8_t *cr = vga->cr;
    const uint8_t  ch = (uint8_t)((cr[0x09] & 0x1Fu) + 1u);
    if (ch == 0) return false;

    const uint16_t vde = (uint16_t)(cr[0x12]
                       | ((cr[0x07] & 0x02u) << 7)
                       | ((cr[0x07] & 0x40u) << 3));

    const uint16_t cols = (uint16_t)cr[0x01] + 1u;
    const uint16_t rows = (uint16_t)((vde + 1u) / ch);

    // Nothing plausible means nothing programmed; say so rather than
    // hand back a shape that would tear the screen up.
    if (cols < 20u || cols > 132u || rows < 10u || rows > 60u) return false;

    out->columns      = (uint8_t)cols;
    out->rows         = (uint8_t)rows;
    out->char_height  = ch;
    out->start_addr   = (uint16_t)(cr[0x0C] << 8 | cr[0x0D]);
    out->cursor_addr  = (uint16_t)(cr[0x0E] << 8 | cr[0x0F]);
    out->cursor_start = cr[0x0A];
    out->cursor_end   = cr[0x0B];
    return true;
}

/*
 * Pack a six-bit-per-channel colour the way the ladder wants it.
 *
 * A VGA DAC entry is 0..63 a channel; this board has two bits, so each is
 * shifted down four. 0xC0 is the sync pair, held inactive.
 */
static uint8_t pack_colour(const uint8_t r6, const uint8_t g6, const uint8_t b6) {
    return (uint8_t)((((r6 >> 4) & 3) << 4) | (((g6 >> 4) & 3) << 2) | ((b6 >> 4) & 3)) | 0xC0u;
}

void vgacard_get_frame(vgacard_frame_t *out) {
    memset(out, 0, sizeof *out);
    if (!vga) return;

    int w = 0, h = 0;
    out->submode     = vga_get_graphics_mode(vga, &w, &h);
    out->width       = w;
    out->height      = h;
    out->line_offset = vga_get_line_offset(vga);
    out->start_addr  = vga_get_start_addr(vga);
    out->line_compare = vga_get_line_compare(vga);
    out->panning     = vga_get_panning(vga);

    uint8_t pal16[48];
    vga_get_palette16(vga, pal16);
    for (int i = 0; i < 16; i++)
        out->palette[i] = pack_colour(pal16[i * 3], pal16[i * 3 + 1], pal16[i * 3 + 2]);
}


bool vgacard_active(void) {
    if (!vga) return false;
    int w, h;
    return vga_get_graphics_mode(vga, &w, &h) != 0;
}
