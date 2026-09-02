/*
 * frank-xt8086 — an RP2350B acting as the whole chipset for a real 8086
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-xt8086
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "osd.h"

#include <stdio.h>
#include <string.h>

#include "audio.h"
#include "graphics.h"
#include "hid_app.h"
#include "setup_menu.h"
#include "state.h"
#include "ui_gfx.h"

extern uint8_t  current_scancode;
extern mc6845_s mc6845;

// app/main.c owns the open image files and the FDC's media mask, so it
// owns reopening them too.
void media_reload(void);

extern uint8_t *vga_text_source;   // drivers/vga/vga.c
extern uint8_t  videomode;
extern uint8_t  fdd_media_mask;

// Our own text page, the same shape as the one the guest owns.
static uint8_t osd_buf[TEXTMODE_COLS * TEXTMODE_ROWS * 2];
static bool    active;

bool osd_active(void) { return active; }

// ---------------------------------------------------------------------------
// Show and hide
// ---------------------------------------------------------------------------

// What the guest's video state was before we took the screen.
static mc6845_s saved_crtc;
static uint8_t  saved_mode;

static void osd_open(void) {
    saved_crtc = mc6845;
    saved_mode = videomode;

    // Force 80x25 text with our own CRTC values. The guest may be in a
    // graphics mode or a 40-column one, and the menu has to be legible
    // whatever it chose -- the renderer reads these registers, not the
    // guest's intent.
    mc6845_init_text_mode();
    graphics_set_mode(TEXTMODE_80x25_COLOR);

    memset(osd_buf, 0, sizeof osd_buf);
    ui_set_target(osd_buf);
    vga_text_source = osd_buf;
    active = true;
}

static void osd_close(void) {
    active = false;
    vga_text_source = NULL;      // the guest's page is visible again
    ui_set_target(NULL);

    mc6845 = saved_crtc;
    graphics_set_mode(saved_mode);
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

/*
 * Wait for a scancode, keeping the machine alive while we do.
 *
 * Core 0's main loop is not running, so anything that would notice its
 * absence has to be pumped here. Audio is the one that cannot wait: a
 * missed block is an audible click, and a run of them is a buzz.
 */
static uint8_t osd_wait_key(void) {
    for (;;) {
        audio_task();
        keyboard_tick();
        if (current_scancode) {
            const uint8_t sc = current_scancode;
            current_scancode = 0;
            return sc;
        }
        // Break codes and modifiers arrive here too; the caller filters.
        tight_loop_contents();
    }
}

// ---------------------------------------------------------------------------
// The menu
// ---------------------------------------------------------------------------

#define OSD_W 58
#define OSD_H 12

typedef struct {
    const char *label;
    char       *path;      // the settings field this row edits
    uint8_t     drive_bit; // which bit of fdd_media_mask, or 0xFF for the HDD
} osd_row_t;

static void draw(const osd_row_t *rows, const int count, const int sel) {
    const int x = (TEXTMODE_COLS - OSD_W) / 2;
    const int y = (TEXTMODE_ROWS - OSD_H) / 2;

    ui_clear(UI_ATTR(UI_DARKGRAY, UI_BLACK));
    ui_shadow(x, y, OSD_W, OSD_H);
    ui_window(x, y, OSD_W, OSD_H, UI_BOX_DOUBLE, "Drives",
              UI_ATTR_WINDOW, UI_ATTR_BORDER);

    for (int i = 0; i < count; i++) {
        const int row = y + 2 + i;
        const bool s  = i == sel;
        if (s) ui_fill(x + 1, row, OSD_W - 2, 1, ' ', UI_ATTR_SEL);
        ui_text(x + 2, row, rows[i].label, s ? UI_ATTR_SEL : UI_ATTR_WINDOW);

        const char *v = rows[i].path[0] ? rows[i].path : "(empty)";
        const int room = OSD_W - 18;
        char buf[64];
        const int len = (int)strlen(v);
        if (len > room) snprintf(buf, sizeof buf, "...%s", v + len - room + 3);
        else            snprintf(buf, sizeof buf, "%s", v);

        ui_text(x + 16, row, buf,
                s ? UI_ATTR_SEL
                  : rows[i].path[0] ? UI_ATTR_VALUE : UI_ATTR_DIM);
    }

    ui_text_center_in(x, OSD_W, y + OSD_H - 3,
                      "changes take effect immediately", UI_ATTR_DIM);
    ui_hint_bar("ENTER change   DEL eject   UP/DOWN move   ESC close");
}

void osd_drive_menu(void) {
    osd_row_t rows[] = {
        { "Floppy A:",    settings.fda, 0 },
        { "Floppy B:",    settings.fdb, 1 },
        { "Hard disk C:", settings.hdd, 0xFF },
    };
    const int count = (int)(sizeof rows / sizeof rows[0]);

    osd_open();

    int sel = 0;
    bool changed = false;
    for (;;) {
        draw(rows, count, sel);

        const uint8_t sc = osd_wait_key();
        if (sc == 0x48) { sel = (sel + count - 1) % count; continue; }  // UP
        if (sc == 0x50) { sel = (sel + 1) % count;         continue; }  // DOWN

        if (sc == 0x53) {                                              // DEL
            rows[sel].path[0] = '\0';
            changed = true;
            continue;
        }
        if (sc == 0x1C) {                                              // ENTER
            if (file_browser(rows[sel].path, 255, ".img")) changed = true;
            continue;
        }
        if (sc == 0x01) break;                                         // ESC
    }

    osd_close();

    if (changed) {
        // Reopening the images is main()'s job: it owns the FIL objects
        // and the mask the FDC reads. Saving is unconditional, so a swap
        // survives the next reboot as well as this session.
        save_settings();
        media_reload();
    }
}
