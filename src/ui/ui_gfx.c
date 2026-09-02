/*
 * frank-xt8086 — an RP2350B acting as the whole chipset for a real 8086
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-xt8086
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ui_gfx.h"

#include <string.h>

#include "state.h"      // VIDEORAM

#define COLS TEXTMODE_COLS
#define ROWS TEXTMODE_ROWS

// One cell is a CP437 code point and an attribute byte, in that order,
// which is the layout the CGA text renderer in drivers/graphics expects.
static inline uint16_t *cell(const int x, const int y) {
    return (uint16_t *)VIDEORAM + (y * COLS + x);
}

static inline bool on_screen(const int x, const int y) {
    return x >= 0 && x < COLS && y >= 0 && y < ROWS;
}

// ---------------------------------------------------------------------------
// Primitives
// ---------------------------------------------------------------------------

void ui_clear(const uint8_t attr) {
    ui_fill(0, 0, COLS, ROWS, ' ', attr);
}

void ui_putc(const int x, const int y, const uint8_t ch, const uint8_t attr) {
    if (!on_screen(x, y)) return;
    *cell(x, y) = (uint16_t)attr << 8 | ch;
}

void ui_fill(int x, int y, int w, int h, const uint8_t ch, const uint8_t attr) {
    // Clip the rectangle rather than each cell: a full-screen fill is 2400
    // cells and an on_screen() test per cell is 2400 branches for a result
    // the caller already knows.
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > COLS) w = COLS - x;
    if (y + h > ROWS) h = ROWS - y;
    if (w <= 0 || h <= 0) return;

    const uint16_t v = (uint16_t)attr << 8 | ch;
    for (int row = 0; row < h; row++) {
        uint16_t *p = cell(x, y + row);
        for (int col = 0; col < w; col++) *p++ = v;
    }
}

void ui_text(const int x, const int y, const char *s, const uint8_t attr) {
    if (y < 0 || y >= ROWS || !s) return;
    int cx = x;
    for (; *s; s++, cx++) {
        if (cx < 0) continue;
        if (cx >= COLS) return;
        *cell(cx, y) = (uint16_t)attr << 8 | (uint8_t)*s;
    }
}

void ui_text_center(const int y, const char *s, const uint8_t attr) {
    ui_text_center_in(0, COLS, y, s, attr);
}

void ui_text_center_in(const int x, const int w, const int y, const char *s,
                       const uint8_t attr) {
    if (!s) return;
    const int len = (int)strlen(s);
    ui_text(x + (w - len) / 2, y, s, attr);
}

// ---------------------------------------------------------------------------
// Windows
// ---------------------------------------------------------------------------

void ui_window(const int x, const int y, const int w, const int h,
               const ui_box_style_t style, const char *title,
               const uint8_t attr, const uint8_t border_attr) {
    if (w < 2 || h < 2) return;

    const uint8_t tl = style == UI_BOX_DOUBLE ? G_DBL_TL : G_SGL_TL;
    const uint8_t tr = style == UI_BOX_DOUBLE ? G_DBL_TR : G_SGL_TR;
    const uint8_t bl = style == UI_BOX_DOUBLE ? G_DBL_BL : G_SGL_BL;
    const uint8_t br = style == UI_BOX_DOUBLE ? G_DBL_BR : G_SGL_BR;
    const uint8_t hz = style == UI_BOX_DOUBLE ? G_DBL_H  : G_SGL_H;
    const uint8_t vt = style == UI_BOX_DOUBLE ? G_DBL_V  : G_SGL_V;

    ui_fill(x + 1, y + 1, w - 2, h - 2, ' ', attr);           // interior
    ui_fill(x + 1, y,         w - 2, 1, hz, border_attr);     // top
    ui_fill(x + 1, y + h - 1, w - 2, 1, hz, border_attr);     // bottom
    ui_fill(x,         y + 1, 1, h - 2, vt, border_attr);     // left
    ui_fill(x + w - 1, y + 1, 1, h - 2, vt, border_attr);     // right

    ui_putc(x,         y,         tl, border_attr);
    ui_putc(x + w - 1, y,         tr, border_attr);
    ui_putc(x,         y + h - 1, bl, border_attr);
    ui_putc(x + w - 1, y + h - 1, br, border_attr);

    if (title && *title) {
        // Padded with spaces so the title sits in a gap in the rule rather
        // than butting straight up against it.
        char buf[COLS + 1];
        const int room = w - 4;
        if (room > 0) {
            int n = (int)strlen(title);
            if (n > room) n = room;
            buf[0] = ' ';
            memcpy(buf + 1, title, (size_t)n);
            buf[n + 1] = ' ';
            buf[n + 2] = '\0';
            ui_text_center_in(x, w, y, buf, UI_ATTR_TITLE);
        }
    }
}

void ui_shadow(const int x, const int y, const int w, const int h) {
    // Darken what is already there rather than painting a colour: the
    // shadow then works over the plasma backdrop and over another window
    // equally, and it costs one attribute rewrite per cell.
    //
    // Two columns to the right and one row below, offset down by one, is
    // the proportion that reads as a shadow on a cell grid twice as tall
    // as it is wide.
    for (int row = y + 1; row < y + h + 1; row++) {
        for (int col = x + w; col < x + w + 2; col++) {
            if (!on_screen(col, row)) continue;
            uint16_t *c = cell(col, row);
            *c = (uint16_t)UI_ATTR(UI_DARKGRAY, UI_BLACK) << 8 | (*c & 0xFF);
        }
    }
    for (int col = x + 2; col < x + w; col++) {
        const int row = y + h;
        if (!on_screen(col, row)) continue;
        uint16_t *c = cell(col, row);
        *c = (uint16_t)UI_ATTR(UI_DARKGRAY, UI_BLACK) << 8 | (*c & 0xFF);
    }
}

void ui_scrollbar(const int x, const int y0, const int y1, const int total,
                  const int visible, const int top, const uint8_t attr) {
    const int track = y1 - y0 + 1;
    if (track <= 0) return;
    if (total <= visible) {                 // nothing to scroll: plain rule
        ui_fill(x, y0, 1, track, G_SHADE_L, attr);
        return;
    }

    ui_fill(x, y0, 1, track, G_SHADE_L, attr);

    // Thumb length in proportion to how much is on screen, never zero, and
    // positioned so the last row of the list puts it against the bottom.
    int thumb = track * visible / total;
    if (thumb < 1) thumb = 1;
    const int span = total - visible;
    const int pos = span > 0 ? (track - thumb) * top / span : 0;

    ui_fill(x, y0 + pos, 1, thumb, G_BLOCK, attr);
    ui_putc(x, y0,       G_ARROW_UP, attr);
    ui_putc(x, y1,       G_ARROW_DN, attr);
}

void ui_hint_bar(const char *s) {
    ui_fill(0, ROWS - 1, COLS, 1, ' ', UI_ATTR_HINT);
    if (s) ui_text_center(ROWS - 1, s, UI_ATTR_HINT);
}

// ---------------------------------------------------------------------------
// Save and restore
// ---------------------------------------------------------------------------

static uint16_t saved[COLS * ROWS];
static bool     saved_valid;

void ui_screen_save(void) {
    memcpy(saved, VIDEORAM, sizeof saved);
    saved_valid = true;
}

void ui_screen_restore(void) {
    if (!saved_valid) return;
    memcpy(VIDEORAM, saved, sizeof saved);
    saved_valid = false;
}

// ---------------------------------------------------------------------------
// Plasma backdrop
// ---------------------------------------------------------------------------

void ui_plasma(const int seed, const int wx, const int wy,
               const int ww, const int wh) {
    // Four shades of grey from the CP437 dither blocks, which give five
    // effective levels counting the blank. Enough for a gradient that
    // moves, on a palette with no greyscale ramp of its own.
    static const uint8_t shade[5] = { ' ', G_SHADE_L, G_SHADE_M, G_SHADE_D, G_BLOCK };

    const bool skip = ww > 0 && wh > 0;

    for (int y = 0; y < ROWS; y++) {
        for (int x = 0; x < COLS; x++) {
            // The window and its shadow are left alone, so an animating
            // backdrop does not make the text on top of it flicker.
            if (skip && x >= wx && x < wx + ww + 2 &&
                        y >= wy && y < wy + wh + 1) continue;

            // A cheap standing-wave field. Not trigonometric: three
            // triangle waves at different periods, summed. Sine would look
            // marginally smoother and cost a table this does not need.
            const int a = ((x * 2 + seed) & 31);
            const int b = ((y * 3 - seed) & 31);
            const int c = ((x + y + seed / 2) & 31);
            const int tri_a = a < 16 ? a : 31 - a;
            const int tri_b = b < 16 ? b : 31 - b;
            const int tri_c = c < 16 ? c : 31 - c;

            const int level = (tri_a + tri_b + tri_c) * 5 / 48;   // 0..4
            const uint8_t ch = shade[level > 4 ? 4 : level];

            // Dark grey on black keeps the backdrop firmly behind the
            // window; a brighter one competes with the text.
            ui_putc(x, y, ch, UI_ATTR(UI_DARKGRAY, UI_BLACK));
        }
    }
}
