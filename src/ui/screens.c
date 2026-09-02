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
// What a poll found. DEL is singled out because it is the one key with a
// meaning here; everything else only says "somebody is watching".
typedef enum { KEY_NONE, KEY_OTHER, KEY_DEL } key_t;

#define XT_SCANCODE_DEL 0x53u
#define XT_SCANCODE_EXT 0xE0u

static key_t poll_key(void) {
    // The Delete key on anything newer than an original XT keyboard is the
    // two-byte sequence E0 53, and the HID driver passes both bytes
    // through. Treating E0 as "some other key" -- which is what this did
    // -- meant DEL dismissed the splash as an ordinary keypress and could
    // never reach SETUP.
    static bool extended;

    keyboard_tick();
    if (current_scancode) {
        const uint8_t sc = current_scancode;
        current_scancode = 0;

        // Traced because "DEL does nothing" is unfalsifiable without
        // knowing what the keyboard actually sent. Only runs while the
        // splash is up, so it costs the guest nothing.
        printf("[key] %02X\n", sc);

        if (sc == XT_SCANCODE_EXT) { extended = true; return KEY_NONE; }

        const bool was_ext = extended;
        extended = false;

        // Break codes have bit 7 set; a key going up is not a keypress.
        if (sc & 0x80u) return KEY_NONE;

        if (sc == XT_SCANCODE_DEL) return KEY_DEL;   // keypad Del, or E0 53
        (void)was_ext;
        return KEY_OTHER;
    }

    const int c = getchar_timeout_us(0);
    if (c == PICO_ERROR_TIMEOUT) return KEY_NONE;
    if (c == 0x7F) return KEY_DEL;                 // some terminals

    if (c == 0x1B) {
        // ESC [ 3 ~ is Delete on the rest of them. Anything else starting
        // with ESC is a cursor key or a bare Escape, and neither is DEL.
        if (getchar_timeout_us(20000) != '[') return KEY_OTHER;
        if (getchar_timeout_us(20000) != '3') return KEY_OTHER;
        (void)getchar_timeout_us(20000);           // the trailing '~'
        return KEY_DEL;
    }
    return KEY_OTHER;
}

static bool any_key(void) { return poll_key() != KEY_NONE; }

/*
 * Throw away input that arrived before anyone was asked a question.
 *
 * Attaching a host to the console is not a keypress, but it looks like
 * one: opening the port toggles DTR and RTS, and the probe puts a byte or
 * a break onto the target's RX as a result. Without this, plugging in a
 * terminal dismisses the splash instantly and drops the operator into
 * SETUP they did not ask for — and on the bench, where the console is
 * attached at exactly the moment the board is reset, that is every boot.
 *
 * Stale HID state goes the same way, for the same reason.
 */
static void drain_input(void) {
    while (getchar_timeout_us(0) != PICO_ERROR_TIMEOUT) { }
    keyboard_tick();
    current_scancode = 0;
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
    ui_text_center_in(x, SPLASH_W, y + 5,
                      "by Mikhail Matveev, xrip, DnCraptor", UI_ATTR_WINDOW);
    ui_text_center_in(x, SPLASH_W, y + 6,
                      "github.com/rh1tech/frank-xt8086", UI_ATTR_WINDOW);

    snprintf(line, sizeof line, "RP2350B, %lu/%lu overclock",
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
    } else if (info->cpu8086_seen) {
        // It drove the bus, just not where an 8086 out of reset should.
        // Worth distinguishing from silence: this is a wiring or a part
        // fault, not an empty socket.
        snprintf(line, sizeof line, "CPU fetched %05lX, expected %05X",
                 (unsigned long)info->cpu8086_addr, 0xFFFF0u);
        ui_text_center_in(x, SPLASH_W, y + 9, line,
                          UI_ATTR(UI_LIGHTRED, UI_WIN_BG));
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

    ui_text_center_in(x, SPLASH_W, y + 12, "Press DEL to enter SETUP",
                      UI_ATTR_DIM);

    // Animate until the hold expires or somebody presses something. The
    // plasma steps once per frame; 16 ms a frame is close enough to the
    // 60 Hz the display is running at that it looks smooth without
    // needing to synchronise to it.
    drain_input();
    const absolute_time_t deadline = make_timeout_time_ms(hold_ms);
    int frame = 0;
    while (absolute_time_diff_us(get_absolute_time(), deadline) > 0) {
        switch (poll_key()) {
            case KEY_DEL:   return true;    // SETUP
            case KEY_OTHER: return false;   // skip the rest of the wait
            default: break;
        }
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

    drain_input();
    const absolute_time_t deadline = make_timeout_time_ms(hold_ms);
    while (absolute_time_diff_us(get_absolute_time(), deadline) > 0) {
        if (any_key()) break;
        sleep_ms(16);
    }
    ui_screen_restore();
}
