/*
 * frank-xt8086 — an RP2350B acting as the whole chipset for a real 8086
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The EGA/VGA card, which is murm386's.
 *
 * vga.c and vga.h in this directory are copied from murm386 unchanged --
 * a complete VGA with the planar write modes, latches, sequencer,
 * graphics controller, attribute controller and CRTC that EGA graphics
 * actually require. Writing that again would have meant reproducing
 * every subtlety of the four-plane write path from the documentation,
 * and there is a working implementation to hand.
 *
 * Keeping the files byte-identical is deliberate: a fix upstream is then
 * a copy, not a merge. Everything this machine needs to bolt them on
 * lives here instead.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * 128K, not the 256K a full VGA has.
 *
 * The buffer holds four planes interleaved, so its size divided by four
 * is how many display addresses exist: 32K here. That covers every mode
 * this machine can usefully run -- 320x200x16 needs 16,000 addresses,
 * 640x200x16 the same, and mode 13h 16,000 -- and leaves 640x350x16
 * short, which would want 56,000.
 *
 * The reason is simply that there is no more SRAM. 256K would fit only by
 * taking it from somewhere that also needs it, and the mode it buys is
 * the one nothing here asks for.
 */
/*
 * 128K, and not sharing its first byte with the text page.
 *
 * The card's memory and the CGA text buffer are both video_memory, and
 * both used to start at offset 0. They are not the same thing laid out
 * the same way: a text cell is two bytes, a character then an attribute,
 * while the card interleaves four planes a byte at a time. Plane 2 of
 * display address n therefore lands on the character byte of text cell
 * 2n+1 -- so when the video BIOS loaded its font into plane 2, as it does
 * at every text mode set, it wrote over every second character on screen.
 * That is the alternating garbage the POST screen was full of.
 *
 * The card now starts above the 64K the CGA, Tandy and Hercules paths
 * use, and 128K of it is 32768 display addresses: enough for 640x350x16,
 * which wants 28000, and for everything smaller.
 */
#define VGACARD_RAM_SIZE  (128u * 1024u)
#define VGACARD_RAM_BASE  (64u * 1024u)   // clear of the text page

void     vgacard_init(void);

// Program the card for a BIOS mode number (0x0D, 0x0E, 0x10, 0x13...).
// False if the mode is not one this card provides.
bool     vgacard_set_bios_mode(uint8_t mode);
bool     vgacard_active(void);        // is an EGA/VGA mode selected?

void     vgacard_port_write(uint16_t port, uint8_t value);
uint8_t  vgacard_port_read(uint16_t port);

void     vgacard_mem_write(uint32_t addr, uint8_t value);
uint8_t  vgacard_mem_read(uint32_t addr);

/*
 * What the scanline renderer needs to draw a frame.
 *
 * The card's memory holds the four planes interleaved, one byte of each
 * per 32-bit word, so a single fetch carries all four bits of eight
 * pixels. That is murm386's layout and QEMU's before it, and it is why
 * the renderer can be as short as it is.
 */
const uint32_t *vgacard_planes(void);

typedef struct {
    int      submode;      // 0 none, 1 CGA4, 2 EGA planar, 3 VGA256, 4 CGA2
    int      width;
    int      height;
    int      line_offset;  // words per line
    uint16_t start_addr;
    int      line_compare;
    uint8_t  panning;
    uint8_t  palette[16];  // already packed for the ladder
} vgacard_frame_t;

/*
 * The text screen's shape, as the card has it.
 *
 * A VGA CRTC does not mean what a 6845 does. Its horizontal register
 * counts from zero, so eighty columns is 79; its vertical display end
 * lives in register 0x12 with two more bits scattered through the
 * overflow register at 0x07, where a 6845 simply has a row count; and
 * its 0x09 carries the maximum scan line in the low five bits with other
 * things above. Handing those raw to a renderer that expects a 6845 puts
 * every row one character short and the screen shears.
 *
 * False when the card has not been programmed yet, so a caller can leave
 * whatever the firmware set up for itself alone.
 */
typedef struct {
    uint8_t  columns;
    uint8_t  rows;
    uint8_t  char_height;
    uint16_t start_addr;    // in characters
    uint16_t cursor_addr;   // in characters
    uint8_t  cursor_start;  // CRTC 0x0A, as written
    uint8_t  cursor_end;    // CRTC 0x0B, as written
} vgacard_text_t;

bool vgacard_text_geometry(vgacard_text_t *out);

/*
 * Re-read the registers that move the picture, at the frame boundary.
 *
 * The start address, the pixel panning and the split-screen line are the
 * three a game changes while it scrolls, and they have to be taken at the
 * same instant for every scanline of a frame or the picture is drawn from
 * two different positions at once. Sampling them from the main loop, as
 * the rest of the frame state is, is not that instant: it lands wherever
 * it lands, so scrolling judders and the screen tears.
 *
 * Worse, the start address is two registers. Software writes 0x0C and
 * 0x0D as separate OUTs and the scanline interrupt can arrive between
 * them, giving a frame drawn from half of the old address and half of the
 * new -- somewhere else entirely, which is what makes a sprite vanish for
 * a frame. Read until the pair is stable.
 *
 * This is murm386's, including the retry, and it is called from the
 * scanline handler rather than from anywhere else.
 */
void vgacard_snap_frame(void);

// Read the card's current state. Called once a frame, not per scanline.
void vgacard_get_frame(vgacard_frame_t *out);

// The copy the scanline renderer draws from, refreshed once a frame so a
// mode change part-way down the screen cannot tear the geometry.
extern vgacard_frame_t vga_frame;

