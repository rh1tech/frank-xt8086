/*
 * frank-xt8086 — an RP2350B acting as the whole chipset for a real 8086
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-xt8086
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "screens.h"

#include <stdio.h>
#include <string.h>

#include <pico/time.h>

#include "console.h"
#include "hid_app.h"
#include "ui_gfx.h"

extern uint8_t current_scancode;   // set by the USB HID keyboard handler

#ifndef FIRMWARE_NAME
#define FIRMWARE_NAME "frank-xt8086"
#endif
#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "0.00"
#endif

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

/*
 * Has anyone pressed anything, on either input device?
 *
 * Both are polled because both are live at once on this board — a USB
 * keyboard on the host port and a terminal on J1. The splash uses this to
 * decide whether an operator is watching.
 */
static bool any_key(void) {
    keyboard_tick();
    if (current_scancode) { current_scancode = 0; return true; }
    return getchar_timeout_us(0) != PICO_ERROR_TIMEOUT;
}

// ---------------------------------------------------------------------------
// Splash
// ---------------------------------------------------------------------------

#define SPLASH_W 52
#define SPLASH_H 15

bool screen_splash(const splash_info_t *info, const uint32_t hold_ms) {
    const int x = (TEXTMODE_COLS - SPLASH_W) / 2;
    const int y = (TEXTMODE_ROWS - SPLASH_H) / 2 - 1;

    char line[SPLASH_W];

    // The window is drawn once and never redrawn. Only the backdrop
    // animates, and ui_plasma() skips the window's rectangle, so the text
    // is stable while the background moves.
    ui_plasma(0, x, y, SPLASH_W, SPLASH_H);
    ui_shadow(x, y, SPLASH_W, SPLASH_H);
    ui_window(x, y, SPLASH_W, SPLASH_H, UI_BOX_DOUBLE, NULL,
              UI_ATTR_WINDOW, UI_ATTR_BORDER);

    ui_text_center_in(x, SPLASH_W, y + 2, "FRANK XT8086",
                      UI_ATTR(UI_YELLOW, UI_WIN_BG));

    snprintf(line, sizeof line, "Version %s", FIRMWARE_VERSION);
    ui_text_center_in(x, SPLASH_W, y + 4, line, UI_ATTR_WINDOW);
    ui_text_center_in(x, SPLASH_W, y + 5, "Mikhail Matveev", UI_ATTR_WINDOW);
    ui_text_center_in(x, SPLASH_W, y + 6,
                      "github.com/rh1tech/frank-xt8086", UI_ATTR_WINDOW);

    snprintf(line, sizeof line, "RP2350B %lu MHz   PSRAM %lu MHz",
             (unsigned long)info->cpu_mhz, (unsigned long)info->psram_mhz);
    ui_text_center_in(x, SPLASH_W, y + 8, line,
                      UI_ATTR(UI_LIGHTCYAN, UI_WIN_BG));

    // The 8086 line is the point of the splash: it is the one component
    // that can be absent, and the difference between "missing" and
    // "broken" is worth an operator seeing before the BIOS runs.
    if (info->cpu8086_present) {
        snprintf(line, sizeof line, "Intel 8086 detected at %lu.%02lu MHz",
                 (unsigned long)(info->cpu8086_khz / 1000u),
                 (unsigned long)((info->cpu8086_khz % 1000u) / 10u));
        ui_text_center_in(x, SPLASH_W, y + 9, line,
                          UI_ATTR(UI_LIGHTGREEN, UI_WIN_BG));
    } else {
        ui_text_center_in(x, SPLASH_W, y + 9, "NO CPU DETECTED",
                          UI_ATTR(UI_LIGHTRED, UI_WIN_BG));
    }

    if (info->sd_ok) {
        ui_text_center_in(x, SPLASH_W, y + 10, "microSD ready",
                          UI_ATTR(UI_LIGHTGREEN, UI_WIN_BG));
    } else {
        ui_text_center_in(x, SPLASH_W, y + 10,
                          "No microSD - built-in bootOS only",
                          UI_ATTR(UI_YELLOW, UI_WIN_BG));
    }

    ui_text_center_in(x, SPLASH_W, y + 12, "Press any key for SETUP",
                      UI_ATTR_DIM);

    // Animate until the hold expires or somebody presses something. The
    // plasma steps once per frame; 16 ms a frame is close enough to the
    // 60 Hz the display is running at that it looks smooth without
    // needing to synchronise to it.
    const absolute_time_t deadline = make_timeout_time_ms(hold_ms);
    int frame = 0;
    while (absolute_time_diff_us(get_absolute_time(), deadline) > 0) {
        if (any_key()) return true;
        ui_plasma(++frame, x, y, SPLASH_W, SPLASH_H);
        sleep_ms(16);
    }
    return false;
}

// ---------------------------------------------------------------------------
// Error and warning
// ---------------------------------------------------------------------------

#define MSG_W 62

// The two differ only in colour, size and what happens afterwards, so
// they share the drawing.
static void draw_message_box(const char *title, const char *message,
                             const char *detail, const char *footer,
                             const uint8_t box_attr, const uint8_t text_attr,
                             const int h) {
    const int x = (TEXTMODE_COLS - MSG_W) / 2;
    const int y = (TEXTMODE_ROWS - h) / 2;

    ui_clear(UI_ATTR(UI_LIGHTGRAY, UI_BLACK));
    ui_shadow(x, y, MSG_W, h);
    ui_window(x, y, MSG_W, h, UI_BOX_DOUBLE, title, box_attr, box_attr);

    ui_text_center_in(x, MSG_W, y + 2, message, text_attr);
    if (detail && *detail) ui_text_center_in(x, MSG_W, y + 4, detail, box_attr);
    if (footer && *footer) ui_text_center_in(x, MSG_W, y + h - 2, footer, box_attr);
}

[[noreturn]] void screen_fatal(const char *title, const char *message,
                               const char *detail) {
    draw_message_box(title, message, detail,
                     "Check the hardware and reset the board.",
                     UI_ATTR_ERROR, UI_ATTR_ERRTEXT, 10);

    // Also to the console. A board with no display attached still has J1,
    // and this is precisely the failure an operator cannot see otherwise.
    printf("\n[xt8086] FATAL: %s\n         %s\n", title, message);
    if (detail && *detail) printf("         %s\n", detail);

    while (true) {
        // Nothing left to do, and nothing safe to do: the 8086 has not
        // been released from reset, so there is no guest to keep alive.
        __wfe();
    }
}

void screen_warning(const char *title, const char *message,
                    const char *detail, const uint32_t hold_ms) {
    ui_screen_save();
    draw_message_box(title, message, detail, "Press any key to continue",
                     UI_ATTR_WARN, UI_ATTR_WARN, 9);

    printf("[xt8086] warning: %s -- %s\n", title, message);
    if (detail && *detail) printf("         %s\n", detail);

    const absolute_time_t deadline = make_timeout_time_ms(hold_ms);
    while (absolute_time_diff_us(get_absolute_time(), deadline) > 0) {
        if (any_key()) break;
        sleep_ms(16);
    }
    ui_screen_restore();
}
