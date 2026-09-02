/*
 * frank-xt8086 — an RP2350B acting as the whole chipset for a real 8086
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-xt8086
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Text-mode UI primitives: windows, shadows, scrollbars, save/restore.
 *
 * Everything here draws into VIDEORAM, the same 80x30 CGA text buffer the
 * 8086 will use once it starts. That is safe only because all of this runs
 * before core 1 releases the CPU from reset — there is exactly one writer
 * at a time, and the handover is the multicore_launch_core1() in main().
 *
 * The style is protea's: small centred windows with a double border and a
 * drop shadow, not the full-screen frames the prototype drew. A dialog
 * with four options should look like a dialog with four options.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "graphics.h"   // TEXTMODE_COLS, TEXTMODE_ROWS

// ---------------------------------------------------------------------------
// Colour
// ---------------------------------------------------------------------------
//
// The CGA attribute byte is bg<<4 | fg, four bits each. The six-bit ladder
// on this board reduces these to 64 colours (see docs/pinout.md), which is
// why the palette below sticks to the sixteen CGA entries rather than
// inventing shades that would collide once they hit the DAC.
#define UI_BLACK        0x0
#define UI_BLUE         0x1
#define UI_GREEN        0x2
#define UI_CYAN         0x3
#define UI_RED          0x4
#define UI_MAGENTA      0x5
#define UI_BROWN        0x6
#define UI_LIGHTGRAY    0x7
#define UI_DARKGRAY     0x8
#define UI_LIGHTBLUE    0x9
#define UI_LIGHTGREEN   0xA
#define UI_LIGHTCYAN    0xB
#define UI_LIGHTRED     0xC
#define UI_LIGHTMAGENTA 0xD
#define UI_YELLOW       0xE
#define UI_WHITE        0xF

#define UI_ATTR(fg, bg) ((uint8_t)(((bg) << 4) | ((fg) & 0x0F)))

// The house style, in one place so a change is one edit.
#define UI_WIN_FG       UI_WHITE
#define UI_WIN_BG       UI_BLUE
#define UI_ATTR_WINDOW  UI_ATTR(UI_WIN_FG, UI_WIN_BG)
#define UI_ATTR_BORDER  UI_ATTR(UI_LIGHTCYAN, UI_WIN_BG)
#define UI_ATTR_TITLE   UI_ATTR(UI_YELLOW, UI_WIN_BG)
#define UI_ATTR_SEL     UI_ATTR(UI_BLACK, UI_LIGHTCYAN)
#define UI_ATTR_DIM     UI_ATTR(UI_DARKGRAY, UI_WIN_BG)
#define UI_ATTR_VALUE   UI_ATTR(UI_LIGHTGREEN, UI_WIN_BG)
#define UI_ATTR_HINT    UI_ATTR(UI_LIGHTGRAY, UI_BLACK)
#define UI_ATTR_ERROR   UI_ATTR(UI_WHITE, UI_RED)
#define UI_ATTR_ERRTEXT UI_ATTR(UI_YELLOW, UI_RED)
#define UI_ATTR_WARN    UI_ATTR(UI_BLACK, UI_BROWN)

// ---------------------------------------------------------------------------
// CP437 box-drawing glyphs
// ---------------------------------------------------------------------------
//
// Named rather than spelled as hex at each use, because 0xC9 in the middle
// of a draw call tells you nothing and ╔ does.
#define G_DBL_TL   0xC9  /* ╔ */
#define G_DBL_TR   0xBB  /* ╗ */
#define G_DBL_BL   0xC8  /* ╚ */
#define G_DBL_BR   0xBC  /* ╝ */
#define G_DBL_H    0xCD  /* ═ */
#define G_DBL_V    0xBA  /* ║ */
#define G_SGL_TL   0xDA  /* ┌ */
#define G_SGL_TR   0xBF  /* ┐ */
#define G_SGL_BL   0xC0  /* └ */
#define G_SGL_BR   0xD9  /* ┘ */
#define G_SGL_H    0xC4  /* ─ */
#define G_SGL_V    0xB3  /* │ */
#define G_SHADE_L  0xB0  /* ░ */
#define G_SHADE_M  0xB1  /* ▒ */
#define G_SHADE_D  0xB2  /* ▓ */
#define G_BLOCK    0xDB  /* █ */
#define G_ARROW_UP 0x18  /* ↑ */
#define G_ARROW_DN 0x19  /* ↓ */
#define G_BULLET   0x07  /* • */

typedef enum { UI_BOX_SINGLE, UI_BOX_DOUBLE } ui_box_style_t;

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------
//
// Every one of these clips to the screen. A caller that computes a window
// position from a string length should not also have to prove the result
// is on-screen, and an off-by-one that scribbles past VIDEORAM would land
// in the middle of whatever the linker put next.

void ui_clear(uint8_t attr);
void ui_putc(int x, int y, uint8_t ch, uint8_t attr);
void ui_fill(int x, int y, int w, int h, uint8_t ch, uint8_t attr);

// Write a string. Not wrapped; clipped at the right edge.
void ui_text(int x, int y, const char *s, uint8_t attr);

// Write a string centred on the screen's width, or within [x, x+w).
void ui_text_center(int y, const char *s, uint8_t attr);
void ui_text_center_in(int x, int w, int y, const char *s, uint8_t attr);

// Border, interior fill and an optional centred title in the top edge.
void ui_window(int x, int y, int w, int h, ui_box_style_t style,
               const char *title, uint8_t attr, uint8_t border_attr);

// Darken the cells two columns right and one row below a window, so it
// reads as floating above whatever is behind it.
void ui_shadow(int x, int y, int w, int h);

// A vertical scrollbar in a window's right border, rows y0..y1 inclusive.
void ui_scrollbar(int x, int y0, int y1, int total, int visible, int top,
                  uint8_t attr);

// A one-line hint bar across the bottom row of the screen.
void ui_hint_bar(const char *s);

// ---------------------------------------------------------------------------
// Save and restore
// ---------------------------------------------------------------------------
//
// One slot, not a stack: the deepest nesting this UI has is a dialog over
// a menu, and a stack would be a buffer per level of a depth nobody uses.
// Calling save twice without an intervening restore overwrites the first.
void ui_screen_save(void);
void ui_screen_restore(void);

// ---------------------------------------------------------------------------
// Backdrop
// ---------------------------------------------------------------------------
//
// A greyscale plasma, in the manner of murm386's splash. `seed` advances
// the pattern; the rectangle (wx, wy, ww, wh) is left untouched so the
// window on top of it does not flicker as the background animates. Pass
// ww or wh <= 0 to fill the whole screen.
void ui_plasma(int seed, int wx, int wy, int ww, int wh);
