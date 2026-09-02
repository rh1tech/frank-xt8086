// setup_min.c -- minimal, compact SETUP menu + file browser
#include "setup_menu.h"
#include "graphics.h"
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

#define BROWSER_WIDTH  60
#define BROWSER_HEIGHT 20
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
    {"Features:",  .colors = {14, 1}},
    {"  CPU Frequency:           %s", ARRAY, &settings.cpu_freq_index, nullptr, 2, {"1 MHz", "4.75 MHz", "6 MHz"}},
    {"  IBM PCjr/Tandy mode:     %s", ARRAY, &settings.tandy_enabled, nullptr, 1, {"NO", "YES"}},
    {""},
    {"Storage devices:",  .colors = {14, 1}},
    {"  Floppy#1:               %s", STRING, settings.fda, nullptr, 255},
    {"  Floppy #2:               %s", STRING, settings.fdb, nullptr, 255},
    {"  Hard drive:              %s", STRING, settings.hdd, nullptr, 255},
    {"At least Floppy #1 or HDD should be selected to bootup!", NONE, NULL, nullptr, .colors = {3, 1}},
    {""},
    {"  Save Settings And Exit  ", EXIT, .colors = {10, 1 }}
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

/* How long SETUP waits for a human before booting on its own. Long enough
 * to catch, short enough that a headless board is not visibly stuck. */
#define SETUP_AUTOBOOT_MS 4000u

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

bool file_browser(char *selected_path, const uint8_t out_len, const char *filter) {
    DIR dir;
    FILINFO fno;
    FE files[BROWSER_MAX_FILES];
    char current_path[256];
    uint8_t item_count = 0;
    uint8_t cur = 0, scroll = 0;

    strcpy(current_path, "/XT");

    while (1) {
        item_count = 0;
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
            bool isdir = (fno.fattrib & AM_DIR) != 0;
            if (!isdir && filter) {
                const char *ext = strrchr(fno.fname, '.');
                if (!ext || strcmp(ext, filter) != 0) continue;
            }
            strncpy(files[item_count].name, fno.fname, sizeof(files[item_count].name) - 1);
            files[item_count].name[sizeof(files[item_count].name) - 1] = '\0';
            files[item_count].is_dir = isdir;
            item_count++;
        }
        f_closedir(&dir);

        if (item_count == 0) {
            strncpy(files[0].name, "..", sizeof(files[0].name));
            files[0].is_dir = true;
            item_count = 1;
        }
        if (cur >= item_count) cur = scroll = 0;
        if (scroll > cur) scroll = cur;
        if (cur >= scroll + BROWSER_MAX_VISIBLE) scroll = cur - BROWSER_MAX_VISIBLE + 1;

        /* build title */
        char title[BROWSER_WIDTH + 1];
        if (strlen(current_path) > BROWSER_WIDTH - 4)
            snprintf(title, sizeof(title), "...%s", current_path + strlen(current_path) - (BROWSER_WIDTH - 7));
        else
            snprintf(title, sizeof(title), "%s", current_path);

        draw_window(title, "ENTER: Select  ESC: Cancel", (TEXTMODE_COLS - BROWSER_WIDTH) / 2, (TEXTMODE_ROWS - BROWSER_HEIGHT) / 2, BROWSER_WIDTH,
                    BROWSER_HEIGHT);

        /* draw visible items */
        for (uint8_t i = 0; i < BROWSER_MAX_VISIBLE && (scroll + i) < item_count; ++i) {
            uint8_t idx = scroll + i;
            char line[BROWSER_WIDTH + 1];
            if (files[idx].is_dir) snprintf(line, sizeof(line), "  [%s]", files[idx].name);
            else snprintf(line, sizeof(line), "  %s", files[idx].name);
            size_t len = strlen(line);
            const size_t max_len = BROWSER_WIDTH - 4;
            if (len > max_len) {
                line[max_len] = '\0';
                len = max_len;
            }
            while (len < max_len) line[len++] = ' ';
            line[len] = '\0';
            uint8_t fg = (idx == cur) ? 0 : (files[idx].is_dir ? 11 : 15);
            uint8_t bg = (idx == cur) ? 15 : 1;
            draw_text(line, (TEXTMODE_COLS - BROWSER_WIDTH) / 2 + 2, (TEXTMODE_ROWS - BROWSER_HEIGHT) / 2 + 3 + i, fg, bg);
        }

        const uint8_t scancode = wait_scancode();

        if (scancode == 0x48) {
            if (cur > 0) cur--;
            if (cur < scroll) scroll = cur;
        } // UP
        else if (scancode == 0x50) {
            if (cur < item_count - 1) {
                cur++;
                if (cur >= scroll + BROWSER_MAX_VISIBLE) scroll = cur - BROWSER_MAX_VISIBLE + 1;
            }
        } // DOWN
        else if (scancode == 0x1C) {
            // ENTER
            if (files[cur].is_dir) {
                if (strcmp(files[cur].name, "..") == 0) {
                    char *p = strrchr(current_path, '/');
                    if (p && p != current_path) *p = '\0';
                    else strcpy(current_path, "/");
                } else {
                    if (strcmp(current_path, "/") != 0) strncat(current_path, "/", sizeof(current_path) - strlen(current_path) - 1);
                    strncat(current_path, files[cur].name, sizeof(current_path) - strlen(current_path) - 1);
                }
                cur = scroll = 0;
            } else {
                // build path
                if (selected_path && out_len) {
                    if (strcmp(current_path, "/") == 0)
                        snprintf(selected_path, out_len, "/%s", files[cur].name);
                    else
                        snprintf(selected_path, out_len, "%s/%s", current_path, files[cur].name);
                }
                return true;
            }
        } else if (scancode == 0x1B) {
            // ESC
            if (selected_path && out_len) selected_path[0] = '\0';
            return false;
        }
    }
}



static void draw_menu_item(const MenuItem *item, uint8_t y, bool selected) {
    char buf[TEXTMODE_COLS + 1];
    uint8_t fg = item->colors.fg_color;
    uint8_t bg = item->colors.bg_color;

    if (item->type != NONE) {
        if (selected) {
            fg = 0;
            bg = 15;
        } else if (item->type != EXIT) {
            fg = 15;
            bg = 1;
        }
    }
    if (item->type == ARRAY) {
        const char *val = item->value_list[*(uint8_t *) item->value];
        snprintf(buf, sizeof(buf), item->text, val);
    } else if (item->type == STRING) {
        const char *v = (char *) item->value;
        snprintf(buf, sizeof(buf), item->text, v && v[0] ? v : "<no image selected>");
    } else {
        snprintf(buf, sizeof(buf), "%s", item->text);
    }
    draw_text(buf, 2, y, fg, bg);
}

/* ----------------- public: setup_menu ----------------- */
void setup_menu(void) {
    settings_s backup = settings;
    clear_screen();

    char title[TEXTMODE_COLS + 1];
    char footer[TEXTMODE_COLS + 1];
    snprintf(title, sizeof(title), "RP8086 SETUP v1.0");

    uint8_t current = 1; // first editable item
    bool running = true;
    bool redraw = true;
    bool first_input = true;

    while (running) {
        if (redraw) {
            const MenuItem *mi = &menu_items[current];
            const char *f = footers[mi->type];
            snprintf(footer, sizeof(footer), "%s", f ? f : footers[NONE]);
            draw_window(title, footer, 0, 0, TEXTMODE_COLS, TEXTMODE_ROWS);

            uint8_t y = 3;
            for (uint8_t i = 0; i < MENU_COUNT; i++) {
                draw_menu_item(&menu_items[i], y++, (i == current));
            }

            // SETUP is a stop, not a destination: a board with nothing
            // plugged into it should still reach the BIOS. The prototype
            // only auto-booted in its serial-console build, which meant a
            // release image on a board with no keyboard sat on this menu
            // for ever. Unconditional now, and announced, because a boot
            // that continues on its own has to say that it will.
            if (first_input) {
                char hint[64];
                snprintf(hint, sizeof(hint),
                         "[autoboot in %us -- press any key to stay in SETUP]",
                         SETUP_AUTOBOOT_MS / 1000u);
                draw_text(hint, 2, TEXTMODE_ROWS - 2, 14, 1);
            }
            redraw = false;
        }

        uint8_t scancode;
        if (first_input) {
            scancode = wait_scancode_timeout(SETUP_AUTOBOOT_MS);
            first_input = false;
            if (scancode == 0) {
                // Nobody is here. Take the stored settings and boot.
                save_settings();
                running = false;
                break;
            }
            redraw = true;  // repaint to clear the autoboot hint
        } else {
            scancode = wait_scancode();
        }

        const MenuItem *mi = &menu_items[current];
        if (scancode == 0x48) {
            menu_move(&current, -1);
            redraw = true;
        } // UP
        else if (scancode == 0x50) {
            menu_move(&current, +1);
            redraw = true;
        } // DOWN
        else if (scancode == 0x4B) {
            if (mi->type == ARRAY) {
                cycle_array(mi, -1);
                redraw = true;
            }
        } // LEFT
        else if (scancode == 0x4D) {
            if (mi->type == ARRAY) {
                cycle_array(mi, +1);
                redraw = true;
            }
        } // RIGHT
        else if (scancode == 0x1C) {
            // ENTER
            if (mi->type == EXIT) {
                save_settings();
                running = false;
            } else if (mi->type == ARRAY) {
                cycle_array(mi, +1);
                redraw = true;
            } else if (mi->type == STRING) {
                // browse and store into the string buffer
                if (file_browser((char *) mi->value, mi->max_value, ".img")) {
                    // selection already written by browser into mi->value
                }
                redraw = true;
            }
        } else if (scancode == 0x1B) {
            // ESC
            settings = backup;
            running = false;
        }
    }

    clear_screen();
}
