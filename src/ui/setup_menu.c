// setup_min.c -- minimal, compact SETUP menu + file browser
#include "setup_menu.h"
#include "graphics.h"
#include "ui_gfx.h"
#include "state.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <pico/time.h>
#include "ff.h"
#include "pico/stdio.h"
#ifndef DEBUG
#include "hid_app.h"
#endif

// BROWSER_WIDTH/HEIGHT are gone: the window sizes itself from
// BROWSER_MAX_VISIBLE now, so the two could not disagree.
#define BROWSER_MAX_VISIBLE 14
#define BROWSER_MAX_FILES 50
#define CONFIG_FILE "/XT/config.sys"

extern uint8_t current_scancode;

/* ----------------- Settings ----------------- */
settings_s settings = {
    .version = SETTINGS_VERSION,
    .tandy_enabled = 0,
    .cpu_freq_index = 2,  // По умолчанию 6MHz
    .fda = "/XT/fdd.img",
    .fdb = "",
    .hdd = "/XT/hdd.img",
};

/* --------- simplified menu description --------- */
static const MenuItem menu_items[] = {
    {"Machine",         .colors = {UI_YELLOW, UI_WIN_BG}},
    {"CPU frequency",    ARRAY, &settings.cpu_freq_index, nullptr, 2, {"1 MHz", "4.75 MHz", "6 MHz"}},
    {"PCjr/Tandy mode",  ARRAY, &settings.tandy_enabled,  nullptr, 1, {"No", "Yes"}},
    {""},
    {"Drives",          .colors = {UI_YELLOW, UI_WIN_BG}},
    {"Floppy A:",        STRING, settings.fda, nullptr, 255},
    {"Floppy B:",        STRING, settings.fdb, nullptr, 255},
    {"Hard disk C:",     STRING, settings.hdd, nullptr, 255},
    {""},
    {"Save and exit",    EXIT, .colors = {UI_LIGHTGREEN, UI_WIN_BG}}
};
#define MENU_COUNT (sizeof(menu_items)/sizeof(menu_items[0]))

/* ----------------- small helpers ----------------- */
static inline void clear_screen(void) {
    memset(VIDEORAM, 0, TEXTMODE_COLS * 2 * TEXTMODE_ROWS);
}

/* cycle array value by dir = -1 or +1 */
static inline void cycle_array(const MenuItem *it, int dir) {
    if (!it || it->type != ARRAY) return;
    uint8_t *value = (uint8_t *) it->value;
    uint8_t max = it->max_value;
    uint8_t size = max + 1;
    *value = (uint8_t) (((*value + size) + dir) % size);
}

/* menu navigation skip NONE */
static inline void menu_move(uint8_t *current, const int direction) {
    uint8_t item = *current;
    do {
        item = (uint8_t) ((item + MENU_COUNT + direction) % MENU_COUNT);
    } while (menu_items[item].type == NONE);
    *current = item;
}

/* One scancode, from whichever input device produced it first.
 *
 * On the prototype these were two mutually exclusive builds — the USB
 * controller was either a HID host or the console's CDC device, never
 * both, so the reader was #ifdef'd into a console half and a HID half.
 * This board's console is UART0 on J1, so both are live at once and the
 * reader polls both: a USB keyboard and a terminal on the probe are
 * equally good ways to drive SETUP, and neither has to be present.
 *
 * Returns 0 when timeout_ms elapses. timeout_ms == 0 blocks forever.
 *
 * The ANSI decoding is deliberately eager: an escape sequence has to be
 * consumed in one call, or the tail of one arrow key surfaces as garbage
 * on the next read.
 */
#define SETUP_POLL_US 16667u   /* one frame */

static uint8_t wait_scancode_timeout(uint32_t timeout_ms) {
    const absolute_time_t deadline = (timeout_ms > 0)
        ? make_timeout_time_ms(timeout_ms)
        : at_the_end_of_time;

    while (1) {
        // USB HID first: it delivers a real XT scancode and needs no
        // translation, so a keyboard beats a terminal on a tie.
        keyboard_tick();
        if (current_scancode) {
            const uint8_t s = current_scancode;
            current_scancode = 0;
            return s;
        }

        const int64_t remain_us = (timeout_ms > 0)
            ? absolute_time_diff_us(get_absolute_time(), deadline)
            : (int64_t)SETUP_POLL_US;
        if (timeout_ms > 0 && remain_us <= 0) return 0;

        const uint32_t poll_us = (remain_us < (int64_t)SETUP_POLL_US)
            ? (uint32_t)remain_us : SETUP_POLL_US;

        int ch = getchar_timeout_us(poll_us);
        if (ch == PICO_ERROR_TIMEOUT) continue;
        if (ch == '\r' || ch == '\n') return 0x1C;   /* ENTER */

        if (ch == 0x1B) {
            /* Bare ESC, or the start of a cursor-key sequence. 50 ms is
             * long enough for the rest of one to arrive over a 115200
             * line and short enough not to feel like a hang. */
            ch = getchar_timeout_us(50000);
            if (ch == PICO_ERROR_TIMEOUT) return 0x01;   /* ESC */
            if (ch == '[' || ch == 'O') {
                const int c2 = getchar_timeout_us(50000);
                if (c2 == 'A') return 0x48;   /* UP    */
                if (c2 == 'B') return 0x50;   /* DOWN  */
                if (c2 == 'C') return 0x4D;   /* RIGHT */
                if (c2 == 'D') return 0x4B;   /* LEFT  */
            }
            continue;
        }

        return (uint8_t)ch;
    }
}

static inline uint8_t wait_scancode(void) {
    return wait_scancode_timeout(0);
}

/* ----------------- save/load: binary dump ----------------- */

bool save_settings(void) {
    FIL f;
    UINT bw;

    // Устанавливаем текущую версию перед сохранением
    settings.version = SETTINGS_VERSION;

    if (f_open(&f, CONFIG_FILE, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK)
        return false;

    f_write(&f, &settings, sizeof(settings), &bw);
    f_close(&f);

    return bw == sizeof(settings);
}

bool load_settings(void) {
    FIL f;
    UINT br;
    settings_s temp_settings;

    if (f_open(&f, CONFIG_FILE, FA_READ) != FR_OK)
        return false;

    // Читаем настройки во временную структуру
    f_read(&f, &temp_settings, sizeof(temp_settings), &br);
    f_close(&f);

    // Проверяем размер файла и версию структуры
    if (br != sizeof(settings) || temp_settings.version != SETTINGS_VERSION) {
        // Версия не совпадает или размер не тот - используем настройки по умолчанию
        return false;
    }

    // Версия совпадает - копируем настройки
    settings = temp_settings;
    return true;
}


typedef struct {
    char name[64];
    bool is_dir;
} FE;

/*
 * Pick an image, or eject.
 *
 * Two changes from the prototype beyond the frame it draws in.
 *
 * ESC used to write an empty string into the caller's buffer and return
 * false, which the caller ignored — so cancelling a browse silently
 * ejected the drive, and that was the only way to eject at all. ESC now
 * leaves the selection exactly as it was.
 *
 * Ejecting is a list entry instead. It is the first row, above "..", so
 * it is visible rather than a key you have to know about; DEL on the
 * SETUP row does the same thing for anyone who does know.
 *
 * Returns true if the selection changed.
 */
#define BROWSER_W 54
#define BROWSER_H (BROWSER_MAX_VISIBLE + 4)

bool file_browser(char *selected_path, const uint8_t out_len, const char *filter) {
    DIR dir;
    FILINFO fno;
    FE files[BROWSER_MAX_FILES];
    char current_path[256];
    uint8_t item_count = 0;
    uint8_t cur = 0, scroll = 0;

    const int wx = (TEXTMODE_COLS - BROWSER_W) / 2;
    const int wy = (TEXTMODE_ROWS - BROWSER_H) / 2 - 1;

    strcpy(current_path, "/XT");

    while (1) {
        item_count = 0;

        // Row zero, always: eject. is_dir false and an empty name is the
        // marker; nothing on a FAT volume can collide with it.
        files[item_count].name[0] = '\0';
        files[item_count].is_dir  = false;
        item_count++;

        if (strcmp(current_path, "/") != 0) {
            strncpy(files[item_count].name, "..", sizeof(files[item_count].name));
            files[item_count].is_dir = true;
            item_count++;
        }

        if (f_opendir(&dir, current_path) != FR_OK) {
            strcpy(current_path, "/");
            f_opendir(&dir, current_path);
        }

        while (item_count < BROWSER_MAX_FILES) {
            if (f_readdir(&dir, &fno) != FR_OK || fno.fname[0] == 0) break;
            if (fno.fname[0] == '.') continue;
            const bool isdir = (fno.fattrib & AM_DIR) != 0;
            if (!isdir && filter) {
                const char *ext = strrchr(fno.fname, '.');
                // Case-insensitively: DOS tools write .IMG as often as .img
                // and the prototype's strcmp hid half the images on a card.
                if (!ext || strcasecmp(ext, filter) != 0) continue;
            }
            strncpy(files[item_count].name, fno.fname, sizeof(files[item_count].name) - 1);
            files[item_count].name[sizeof(files[item_count].name) - 1] = '\0';
            files[item_count].is_dir = isdir;
            item_count++;
        }
        f_closedir(&dir);

        if (cur >= item_count) cur = scroll = 0;
        if (scroll > cur) scroll = cur;
        if (cur >= scroll + BROWSER_MAX_VISIBLE) scroll = cur - BROWSER_MAX_VISIBLE + 1;

        char title[BROWSER_W];
        const int room = BROWSER_W - 6;
        if ((int)strlen(current_path) > room)
            snprintf(title, sizeof title, "...%s", current_path + strlen(current_path) - room + 3);
        else
            snprintf(title, sizeof title, "%s", current_path);

        // Repaint the backdrop first. The browser is narrower than the
        // SETUP window behind it, so without this the parent's left edge
        // pokes out past it as a column of half-words.
        ui_plasma(0, 0, 0, 0, 0);
        ui_shadow(wx, wy, BROWSER_W, BROWSER_H);
        ui_window(wx, wy, BROWSER_W, BROWSER_H, UI_BOX_DOUBLE, title,
                  UI_ATTR_WINDOW, UI_ATTR_BORDER);

        for (uint8_t i = 0; i < BROWSER_MAX_VISIBLE && (scroll + i) < item_count; ++i) {
            const uint8_t idx = scroll + i;
            const int     y   = wy + 2 + i;
            const bool    sel = idx == cur;
            const bool    eject = !files[idx].is_dir && !files[idx].name[0];

            if (sel) ui_fill(wx + 1, y, BROWSER_W - 2, 1, ' ', UI_ATTR_SEL);

            char line[BROWSER_W];
            if (eject)                   snprintf(line, sizeof line, "%c Eject - no image", G_BULLET);
            else if (files[idx].is_dir)  snprintf(line, sizeof line, "[%s]", files[idx].name);
            else                         snprintf(line, sizeof line, "%s", files[idx].name);

            const uint8_t attr = sel      ? UI_ATTR_SEL
                               : eject    ? UI_ATTR(UI_YELLOW, UI_WIN_BG)
                               : files[idx].is_dir ? UI_ATTR(UI_LIGHTCYAN, UI_WIN_BG)
                                                   : UI_ATTR_WINDOW;
            ui_text(wx + 2, y, line, attr);
        }

        ui_scrollbar(wx + BROWSER_W - 1, wy + 1, wy + BROWSER_H - 2,
                     item_count, BROWSER_MAX_VISIBLE, scroll, UI_ATTR_BORDER);

        ui_hint_bar("ENTER select   UP/DOWN move   ESC cancel");

        const uint8_t scancode = wait_scancode();

        if (scancode == 0x48) {                       // UP
            if (cur > 0) cur--;
            if (cur < scroll) scroll = cur;
        } else if (scancode == 0x50) {                // DOWN
            if (cur < item_count - 1) {
                cur++;
                if (cur >= scroll + BROWSER_MAX_VISIBLE) scroll = cur - BROWSER_MAX_VISIBLE + 1;
            }
        } else if (scancode == 0x1C) {                // ENTER
            if (!files[cur].is_dir && !files[cur].name[0]) {
                if (selected_path && out_len) selected_path[0] = '\0';
                return true;                           // ejected
            }
            if (files[cur].is_dir) {
                if (strcmp(files[cur].name, "..") == 0) {
                    char *p = strrchr(current_path, '/');
                    if (p && p != current_path) *p = '\0';
                    else strcpy(current_path, "/");
                } else {
                    if (strcmp(current_path, "/") != 0)
                        strncat(current_path, "/", sizeof(current_path) - strlen(current_path) - 1);
                    strncat(current_path, files[cur].name, sizeof(current_path) - strlen(current_path) - 1);
                }
                cur = scroll = 0;
            } else {
                if (selected_path && out_len) {
                    if (strcmp(current_path, "/") == 0)
                        snprintf(selected_path, out_len, "/%s", files[cur].name);
                    else
                        snprintf(selected_path, out_len, "%s/%s", current_path, files[cur].name);
                }
                return true;
            }
        } else if (scancode == 0x01) {                // ESC — cancel, change nothing
            return false;
        }
    }
}

#define SETUP_W 58
#define SETUP_H ((int)MENU_COUNT + 4)

// Where the value column starts, measured from the window's left edge.
#define SETUP_VAL_X 20

// The value a drive shows when nothing is loaded. Also what EJECT leaves
// behind, so the two states are indistinguishable — which they are.
#define NO_MEDIA "(empty)"

static void draw_menu_item(const MenuItem *item, const int wx, const int y,
                           const bool selected) {
    const bool heading = item->type == NONE && item->text[0];
    const int  lx      = wx + 2;

    if (!item->text[0]) return;                       // spacer

    if (heading) {
        ui_text(lx - 1, y, item->text, UI_ATTR(item->colors.fg_color,
                                               item->colors.bg_color));
        return;
    }

    // The highlight spans the full inner width, so a selected row reads as
    // a bar rather than as a differently-coloured word.
    const uint8_t attr = selected ? UI_ATTR_SEL : UI_ATTR_WINDOW;
    if (selected) ui_fill(wx + 1, y, SETUP_W - 2, 1, ' ', attr);

    ui_text(lx, y, item->text, attr);

    const char *val = NULL;
    char buf[48];
    if (item->type == ARRAY) {
        val = item->value_list[*(uint8_t *)item->value];
    } else if (item->type == STRING) {
        const char *v = (const char *)item->value;
        if (v && v[0]) {
            // Long paths are elided from the left: the filename is what
            // identifies an image, and "/XT/dos/utils/" is not.
            const int room = SETUP_W - SETUP_VAL_X - 3;
            const int len  = (int)strlen(v);
            if (len > room) snprintf(buf, sizeof buf, "...%s", v + len - room + 3);
            else            snprintf(buf, sizeof buf, "%s", v);
        } else {
            snprintf(buf, sizeof buf, NO_MEDIA);
        }
        val = buf;
    }

    if (val) {
        const uint8_t vattr = selected ? UI_ATTR_SEL
                            : item->type == STRING && !((const char *)item->value)[0]
                              ? UI_ATTR_DIM : UI_ATTR_VALUE;
        ui_text(wx + SETUP_VAL_X, y, val, vattr);
    }
}

/* ----------------- public: setup_menu ----------------- */
void setup_menu(void) {
    const settings_s backup = settings;

    const int wx = (TEXTMODE_COLS - SETUP_W) / 2;
    const int wy = (TEXTMODE_ROWS - SETUP_H) / 2 - 1;

    uint8_t current = 1;    // first editable item
    bool running = true;
    bool redraw  = true;

    while (running) {
        if (redraw) {
            // Full repaint, no skip rectangle. The skip exists so an
            // animating backdrop does not flicker the window on top of
            // it; these screens are static, and skipping the window's own
            // shadow left fragments of the browser that had been drawn
            // over the same rows.
            ui_plasma(0, 0, 0, 0, 0);
            ui_shadow(wx, wy, SETUP_W, SETUP_H);
            ui_window(wx, wy, SETUP_W, SETUP_H, UI_BOX_DOUBLE, "SETUP",
                      UI_ATTR_WINDOW, UI_ATTR_BORDER);

            for (uint8_t i = 0; i < MENU_COUNT; i++)
                draw_menu_item(&menu_items[i], wx, wy + 2 + i, i == current);

            // The hint bar names only the keys that do something on the
            // row you are actually on, which is the whole reason it is
            // redrawn per selection rather than written once.
            switch (menu_items[current].type) {
                case ARRAY:
                    ui_hint_bar("LEFT/RIGHT or ENTER change   UP/DOWN move   ESC discard");
                    break;
                case STRING:
                    ui_hint_bar("ENTER browse   DEL eject   UP/DOWN move   ESC discard");
                    break;
                case EXIT:
                    ui_hint_bar("ENTER save and boot   UP/DOWN move   ESC discard");
                    break;
                default:
                    ui_hint_bar("UP/DOWN move   ESC discard");
                    break;
            }
            redraw = false;
        }

        // No timeout. Reaching SETUP at all now takes a deliberate keypress
        // on the splash, so booting out from under someone who is already
        // in it would be wrong; main() asks "is anyone there" once, before.
        const uint8_t scancode = wait_scancode();
        const MenuItem *mi = &menu_items[current];

        switch (scancode) {
            case 0x48: menu_move(&current, -1); redraw = true; break;   // UP
            case 0x50: menu_move(&current, +1); redraw = true; break;   // DOWN

            case 0x4B: if (mi->type == ARRAY) { cycle_array(mi, -1); redraw = true; } break;
            case 0x4D: if (mi->type == ARRAY) { cycle_array(mi, +1); redraw = true; } break;

            case 0x53:      // DEL — eject
                if (mi->type == STRING) {
                    ((char *)mi->value)[0] = '\0';
                    redraw = true;
                }
                break;

            case 0x1C:      // ENTER
                if (mi->type == EXIT) {
                    save_settings();
                    running = false;
                } else if (mi->type == ARRAY) {
                    cycle_array(mi, +1);
                    redraw = true;
                } else if (mi->type == STRING) {
                    file_browser((char *)mi->value, mi->max_value, ".img");
                    redraw = true;
                }
                break;

            case 0x01:      // ESC — discard
                settings = backup;
                running = false;
                break;

            default: break;
        }
    }

    ui_clear(UI_ATTR(UI_LIGHTGRAY, UI_BLACK));
}
