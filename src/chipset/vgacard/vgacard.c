/*
 * frank-xt8086 — an RP2350B acting as the whole chipset for a real 8086
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "vgacard.h"

#include <stdio.h>
#include <string.h>

#include <pico/time.h>

#include "vga.h"
#include "vgamodes.h"

// The card's memory. Four 64K planes, which is what a VGA has and what
// murm386's vga.c indexes.
static uint8_t vga_ram[VGACARD_RAM_SIZE] __attribute__((aligned(4)));

static VGAState *vga;

/*
 * vga.c wants a microsecond clock for cursor blink and retrace timing.
 * It is the only thing it asks of the host besides memory.
 */
uint32_t get_uticks(void) { return (uint32_t)time_us_64(); }

void vgacard_init(void) {
    memset(vga_ram, 0, sizeof vga_ram);
    // No framebuffer: this machine draws from the card's memory a
    // scanline at a time, so the width and height given here only have
    // to be something vga.c will accept.
    vga = vga_init((char *)vga_ram, (int)sizeof vga_ram, NULL, 640, 480);
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
    memset(vga_ram, 0, sizeof vga_ram);

    int w = 0, h = 0;
    printf("[vga] mode %02X -> submode %d %dx%d\n",
           mode, vga_get_graphics_mode(vga, &w, &h), w, h);
    return true;
}

void vgacard_port_write(const uint16_t port, const uint8_t value) {
    if (vga) vga_ioport_write(vga, port, value);
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
