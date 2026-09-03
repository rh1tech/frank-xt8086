/*
 * frank-xt8086 — an RP2350B acting as the whole chipset for a real 8086
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-xt8086
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The serial console: a text-mode mirror of the screen, and a way to type
 * at the 8086 without a keyboard plugged in.
 *
 * On the prototype this was an either-or with USB: the console needed the
 * USB controller as a CDC device, and a HID keyboard needed it as a host,
 * so a build got one or the other. This board breaks that tie — J1 is a
 * real UART on GP44/GP45 (UART0), stdio goes there, and the USB controller
 * is free to be a host all the time. Both halves of this header are now
 * live in the same build.
 *
 * SERIAL_CONSOLE=0 keeps stdio and printf but drops the screen mirror and
 * the keystroke injection, which is what a release image wants: the mirror
 * repaints 2000 characters every frame and that is a lot of UART for a
 * feature nobody is watching.
 */
#pragma once

#include <pico/stdio.h>
#include <pico/bootrom.h>   // the console's 'B' key drops to BOOTSEL

#include "state.h"      // VIDEORAM
#include "graphics.h"   // TEXTMODE_80x25_COLOR
#include "uart16550.h"  // the emulated COM1 that CTTY mode types into

// A header that uses a symbol should include what declares it. This one
// referenced `uart` and got away with it because main.c was its only
// includer and included uart16550.h first; the moment screens.c included
// it too, the SERIAL_CONSOLE build stopped compiling.

// The USB HID host is always compiled in now; see the note above.
#include "hid_app.h"
#include "tusb.h"

#ifndef SERIAL_CONSOLE
#define SERIAL_CONSOLE 0
#endif

#if SERIAL_CONSOLE

bool handleScancode(uint8_t ps2scancode);

// ============================================================================
// Константы для scancodes модификаторов
// ============================================================================
#define SCANCODE_LSHIFT_MAKE  0x2A
#define SCANCODE_LSHIFT_BREAK 0xAA
#define SCANCODE_LCTRL_MAKE   0x1D
#define SCANCODE_LCTRL_BREAK  0x9D
#define SCANCODE_LALT_MAKE    0x38
#define SCANCODE_LALT_BREAK   0xB8
#define SCANCODE_EXTENDED     0xE0
#define SCANCODE_ESC          0x01

// ============================================================================
// ASCII to Scancode (IBM PC/XT Set 1)
// ============================================================================
__force_inline uint8_t ascii_to_scancode(const int ascii) {
    const uint8_t qwerty_map[] = {
        0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, // a-h
        0x17, 0x24, 0x25, 0x26, 0x32, 0x31, 0x18, 0x19, // i-p
        0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, // q-x
        0x15, 0x2C // y-z
    };

    // Letters
    if (ascii >= 'a' && ascii <= 'z') return qwerty_map[ascii - 'a'];
    if (ascii >= 'A' && ascii <= 'Z') return qwerty_map[ascii - 'A'];

    // Digits
    if (ascii >= '0' && ascii <= '9') {
        static const uint8_t digit_map[] = {
            0x0B, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A
        };
        return digit_map[ascii - '0'];
    }

    // Special keys with Shift (возвращаем базовый scancode)
    switch (ascii) {
        case '!': return 0x02; case '@': return 0x03; case '#': return 0x04;
        case '$': return 0x05; case '%': return 0x06; case '^': return 0x07;
        case '&': return 0x08; case '*': return 0x09; case '(': return 0x0A;
        case ')': return 0x0B; case '_': return 0x0C; case '+': return 0x0D;
        case '{': return 0x1A; case '}': return 0x1B; case ':': return 0x27;
        case '"': return 0x28; case '|': return 0x2B; case '<': return 0x33;
        case '>': return 0x34; case '?': return 0x35; case '~': return 0x29;

        case ' ': return 0x39;
        case '\r': case '\n': return 0x1C;
        case '\b': return 0x0E;
        case '\t': return 0x0F;
        case 27: return SCANCODE_ESC;

        // Symbols without Shift
        case '-': return 0x0C; case '=': return 0x0D; case '[': return 0x1A;
        case ']': return 0x1B; case ';': return 0x27; case '\'': return 0x28;
        case '`': return 0x29; case '\\': return 0x2B; case ',': return 0x33;
        case '.': return 0x34; case '/': return 0x35;

        default: return 0x00;
    }
}

// ============================================================================
// Вспомогательные функции
// ============================================================================
__force_inline bool needs_shift(int ascii) {
    return (ascii >= 'A' && ascii <= 'Z') ||
           ascii == '!' || ascii == '@' || ascii == '#' || ascii == '$' ||
           ascii == '%' || ascii == '^' || ascii == '&' || ascii == '*' ||
           ascii == '(' || ascii == ')' || ascii == '_' || ascii == '+' ||
           ascii == '{' || ascii == '}' || ascii == ':' || ascii == '"' ||
           ascii == '|' || ascii == '<' || ascii == '>' || ascii == '?' ||
           ascii == '~';
}

__force_inline bool is_control_exception(int ascii) {
    // Backspace, Tab, Enter, ESC обрабатываются отдельно
    return ascii == 0x08 || ascii == 0x09 || ascii == 0x0D || ascii == 0x1B;
}

__force_inline uint8_t get_ctrl_char_scancode(int ascii) {
    if (ascii <= 0x1A) return ascii_to_scancode('a' + (ascii - 1)); // Ctrl+A-Z
    if (ascii == 0x1C) return 0x2B; // Ctrl+\ (backslash)
    if (ascii == 0x1D) return 0x1B; // Ctrl+]
    if (ascii == 0x1E) return 0x07; // Ctrl+^
    return 0x0C; // 0x1F = Ctrl+_
}

__force_inline void send_scancode_with_delay(uint8_t scancode) {
    handleScancode(scancode);
    sleep_ms(1);
}

/*
 * A key is a press *and* a release.
 *
 * Only the modifiers were ever released here. The key itself went out as a
 * make code and was left down, because the BIOS keyboard buffer acts on
 * make codes and ignores everything else -- so typing at a DOS prompt
 * worked, and that hid this for as long as the console was only ever used
 * to type DOS commands.
 *
 * A game that installs its own INT 9 and tracks key state does care.
 * Dangerous Dave 2 sits on its title screen forever: it saw the key go
 * down, never saw it come up, and so never sees the next press. Its
 * gameplay is unreachable over the console without this, and with it the
 * arrow keys become usable as well -- which is what scrolling needs.
 *
 * An XT break code is the make code with bit 7 set, and an extended key
 * carries the E0 prefix on both halves.
 */
__force_inline void send_key_tap(uint8_t scancode, bool extended) {
    if (extended) send_scancode_with_delay(SCANCODE_EXTENDED);
    send_scancode_with_delay(scancode);
    if (extended) send_scancode_with_delay(SCANCODE_EXTENDED);
    send_scancode_with_delay((uint8_t)(scancode | 0x80));
}

// ============================================================================
// Отправка клавиши с модификаторами
// ============================================================================
__force_inline void send_key(uint8_t modifier, uint8_t scancode, bool is_extended) {
    // ANSI модификаторы: 1=none, 2=Shift, 3=Alt, 4=Shift+Alt, 5=Ctrl, 6=Shift+Ctrl, 7=Alt+Ctrl, 8=All
    bool has_shift = (modifier == 2 || modifier == 4 || modifier == 6 || modifier == 8);
    bool has_alt = (modifier == 3 || modifier == 4 || modifier == 7 || modifier == 8);
    bool has_ctrl = (modifier >= 5 && modifier <= 8);

    // Make-коды модификаторов
    if (has_shift) send_scancode_with_delay(SCANCODE_LSHIFT_MAKE);
    if (has_alt) send_scancode_with_delay(SCANCODE_LALT_MAKE);
    if (has_ctrl) send_scancode_with_delay(SCANCODE_LCTRL_MAKE);

    send_key_tap(scancode, is_extended);

    // Break-коды модификаторов (в обратном порядке)
    if (has_ctrl) send_scancode_with_delay(SCANCODE_LCTRL_BREAK);
    if (has_alt) send_scancode_with_delay(SCANCODE_LALT_BREAK);
    if (has_shift) send_scancode_with_delay(SCANCODE_LSHIFT_BREAK);
}

// Wrapper-функции для удобства
#define send_key_with_modifiers(modifier, scancode) send_key(modifier, scancode, false)
#define send_extended_key_with_modifiers(modifier, scancode) send_key(modifier, scancode, true)

// ============================================================================
// Отправка обычного ASCII символа
// ============================================================================
__force_inline void send_ascii_as_scancode(int ascii) {
    // Обработка Ctrl+буква (control characters 0x01-0x1F)
    if (ascii >= 0x01 && ascii <= 0x1F && !is_control_exception(ascii)) {
        send_scancode_with_delay(SCANCODE_LCTRL_MAKE);
        send_key_tap(get_ctrl_char_scancode(ascii), false);
        send_scancode_with_delay(SCANCODE_LCTRL_BREAK);
        return;
    }

    uint8_t scancode = ascii_to_scancode(ascii);
    if (scancode == 0x00) return;

    // Отправляем с Shift если нужно
    if (needs_shift(ascii)) send_scancode_with_delay(SCANCODE_LSHIFT_MAKE);
    send_key_tap(scancode, false);
    if (needs_shift(ascii)) send_scancode_with_delay(SCANCODE_LSHIFT_BREAK);
}

// ============================================================================
// ANSI Escape Sequence Parser
// ============================================================================
typedef enum {
    ANSI_STATE_NORMAL,
    ANSI_STATE_ESC,
    ANSI_STATE_CSI,
    ANSI_STATE_SS3
} ansi_state_t;

__force_inline void process_ss3_sequence(char c) {
    // SS3 последовательности (ESC O X)
    switch (c) {
        case 'P': send_key_tap(0x3B, false); break; // F1
        case 'Q': send_key_tap(0x3C, false); break; // F2
        case 'R': send_key_tap(0x3D, false); break; // F3
        case 'S': send_key_tap(0x3E, false); break; // F4
        case 'A': send_key_tap(0x48, true);  break; // Up
        case 'B': send_key_tap(0x50, true);  break; // Down
        case 'C': send_key_tap(0x4D, true);  break; // Right
        case 'D': send_key_tap(0x4B, true);  break; // Left
    }
}

__force_inline void process_ansi_sequence(const char *seq, int len) {
    if (len < 1) return;

    /*
     * ESC [ <n> K -- put scancode n on the wire, exactly as given.
     *
     * Uppercase because the CSI parser only ends a sequence on an
     * uppercase letter or a tilde; a lowercase terminator is accumulated
     * and thrown away when the buffer fills, silently.
     *
     * Everything else here sends a whole keystroke: press and release
     * together. That is what typing wants and it is all the console could
     * do, which means it could never hold one key down while pressing
     * another -- no walking right and jumping, so no way to drive a game
     * from a script and watch what the screen does.
     *
     * This sends one byte. Holding right is 224 then 77; letting go is
     * 224 then 205. Two of them make a keystroke, and the caller decides
     * when the second one happens.
     */
    if (seq[len - 1] == 'K') {
        int code = 0;
        for (int i = 0; i < len - 1; i++) {
            if (seq[i] < '0' || seq[i] > '9') return;
            code = code * 10 + (seq[i] - '0');
        }
        if (code > 0 && code < 256) handleScancode((uint8_t)code);
        return;
    }

    // Парсинг модификатора (формат: "1;2A" или "15;5~")
    uint8_t modifier = 1;
    int num = 0;
    int semicolon_pos = -1;

    for (int i = 0; i < len; i++) {
        if (seq[i] == ';') {
            semicolon_pos = i;
            break;
        }
    }

    if (semicolon_pos != -1) {
        modifier = 0;
        for (int i = semicolon_pos + 1; i < len; i++) {
            if (seq[i] >= '0' && seq[i] <= '9') {
                modifier = modifier * 10 + (seq[i] - '0');
            }
        }
        if (modifier == 0) modifier = 1;
    }

    char terminator = seq[len - 1];

    // Таблица для простых терминаторов
    switch (terminator) {
        case 'A': send_extended_key_with_modifiers(modifier, 0x48); return; // Up
        case 'B': send_extended_key_with_modifiers(modifier, 0x50); return; // Down
        case 'C': send_extended_key_with_modifiers(modifier, 0x4D); return; // Right
        case 'D': send_extended_key_with_modifiers(modifier, 0x4B); return; // Left
        case 'H': send_extended_key_with_modifiers(modifier, 0x47); return; // Home
        case 'F': send_extended_key_with_modifiers(modifier, 0x4F); return; // End
        case 'P': send_key_with_modifiers(modifier, 0x3B); return; // F1
        case 'Q': send_key_with_modifiers(modifier, 0x3C); return; // F2
        case 'R': send_key_with_modifiers(modifier, 0x3D); return; // F3
        case 'S': send_key_with_modifiers(modifier, 0x3E); return; // F4
    }

    if (terminator != '~') return;

    // Парсим число для последовательностей ESC[N~
    int end_pos = (semicolon_pos != -1) ? semicolon_pos : len - 1;
    for (int i = 0; i < end_pos; i++) {
        if (seq[i] >= '0' && seq[i] <= '9') {
            num = num * 10 + (seq[i] - '0');
        }
    }

    // Таблица для ESC[N~
    switch (num) {
        case 1: send_extended_key_with_modifiers(modifier, 0x47); break; // Home
        case 2: send_extended_key_with_modifiers(modifier, 0x52); break; // Insert
        case 3: send_extended_key_with_modifiers(modifier, 0x53); break; // Delete
        case 4: send_extended_key_with_modifiers(modifier, 0x4F); break; // End
        case 5: send_extended_key_with_modifiers(modifier, 0x49); break; // Page Up
        case 6: send_extended_key_with_modifiers(modifier, 0x51); break; // Page Down
        case 11: send_key_with_modifiers(modifier, 0x3B); break; // F1
        case 12: send_key_with_modifiers(modifier, 0x3C); break; // F2
        case 13: send_key_with_modifiers(modifier, 0x3D); break; // F3
        case 14: send_key_with_modifiers(modifier, 0x3E); break; // F4
        case 15: send_key_with_modifiers(modifier, 0x3F); break; // F5
        case 17: send_key_with_modifiers(modifier, 0x40); break; // F6
        case 18: send_key_with_modifiers(modifier, 0x41); break; // F7
        case 19: send_key_with_modifiers(modifier, 0x42); break; // F8
        case 20: send_key_with_modifiers(modifier, 0x43); break; // F9
        case 21: send_key_with_modifiers(modifier, 0x44); break; // F10
        case 23: send_key_with_modifiers(modifier, 0x57); break; // F11
        case 24: send_key_with_modifiers(modifier, 0x58); break; // F12
    }
}

// ============================================================================
// Обработка Alt+символ (ESC + символ)
// ============================================================================
__force_inline void process_alt_sequence(int c) {
    // Alt+Ctrl+буква?
    if (c >= 0x01 && c <= 0x1F && !is_control_exception(c)) {
        send_scancode_with_delay(SCANCODE_LALT_MAKE);
        send_scancode_with_delay(SCANCODE_LCTRL_MAKE);
        send_scancode_with_delay(get_ctrl_char_scancode(c));
        send_scancode_with_delay(SCANCODE_LCTRL_BREAK);
        send_scancode_with_delay(SCANCODE_LALT_BREAK);
        return;
    }

    // Обычный Alt+символ
    uint8_t scancode = ascii_to_scancode(c);
    if (scancode == 0x00) return;

    bool shift = needs_shift(c);
    send_scancode_with_delay(SCANCODE_LALT_MAKE);
    if (shift) send_scancode_with_delay(SCANCODE_LSHIFT_MAKE);
    send_scancode_with_delay(scancode);
    if (shift) send_scancode_with_delay(SCANCODE_LSHIFT_BREAK);
    send_scancode_with_delay(SCANCODE_LALT_BREAK);
}

__force_inline void console_init(void) {
    stdio_init_all();
    setvbuf(stdout, NULL, _IONBF, 0);

    // No wait loop: stdio is UART0 on J1, which is up the moment the pins
    // are configured. The prototype's five-second pause was there to let a
    // USB host enumerate a CDC device before the first printf was lost.
    printf("\n[xt8086] serial console on UART0 (GP%d/GP%d)\n",
           UART_TX_PIN, UART_RX_PIN);
}

__force_inline void console_task(const int videomode) {
    static bool video_enabled = false;
    static bool ctty_mode = false;
    static ansi_state_t ansi_state = ANSI_STATE_NORMAL;
    static char ansi_buffer[16];
    static int ansi_buf_len = 0;

    // Рендеринг видео
    if (video_enabled && videomode <= TEXTMODE_80x25_COLOR) {
        printf("\033[H\033[2J\033[3J\033[40m\033[?25l");
        for (int y = 0; y < 25; y++) {
            const uint32_t *framebuffer_line = (uint32_t *) VIDEORAM + __fast_mul(y, 40);
            for (int x = 40; x--;) {
                const uint32_t dword = *framebuffer_line++ & 0x00FF00FF;
                putchar_raw(dword);
                putchar_raw(dword >> 16);
            }
            if (y != 24) {
                putchar_raw(0x0D);
                putchar_raw(0x0A);
            }
        }
    }

    int c = getchar_timeout_us(0);
    if (c == PICO_ERROR_TIMEOUT) return;

    // ═══════════════════════════════════════════════════════════════
    // Обработка ANSI state machine
    // ═══════════════════════════════════════════════════════════════
    if (ansi_state != ANSI_STATE_NORMAL) {
        if (ctty_mode) {
            uart.rbr = (uint8_t) c;
            uart.data_ready = true;
        } else {
            switch (ansi_state) {
                case ANSI_STATE_ESC:
                    ansi_state = ANSI_STATE_NORMAL;
                    if (c == '[') { ansi_state = ANSI_STATE_CSI; ansi_buf_len = 0; }
                    else if (c == 'O') { ansi_state = ANSI_STATE_SS3; }
                    else if (c == 0x1B) { send_scancode_with_delay(SCANCODE_ESC); }
                    else { process_alt_sequence(c); }
                    break;

                case ANSI_STATE_SS3:
                    process_ss3_sequence((char)c);
                    ansi_state = ANSI_STATE_NORMAL;
                    break;

                case ANSI_STATE_CSI:
                    if (ansi_buf_len < (int)sizeof(ansi_buffer) - 1) {
                        ansi_buffer[ansi_buf_len++] = (char)c;
                    }
                    if ((c >= 'A' && c <= 'Z') || c == '~') {
                        ansi_buffer[ansi_buf_len] = '\0';
                        process_ansi_sequence(ansi_buffer, ansi_buf_len);
                        ansi_state = ANSI_STATE_NORMAL;
                        ansi_buf_len = 0;
                    } else if (ansi_buf_len >= (int)sizeof(ansi_buffer) - 1) {
                        ansi_state = ANSI_STATE_NORMAL;
                        ansi_buf_len = 0;
                    }
                    break;

                default: break;
            }
        }
        return;
    }

    // ═══════════════════════════════════════════════════════════════
    // Обработка спецкоманд
    // ═══════════════════════════════════════════════════════════════
    if (c == '`') {
        video_enabled = !video_enabled;
    } else if (c == 'C') {
        ctty_mode = !ctty_mode;
        printf("\033[2J\033[H");
        if (ctty_mode) {
            video_enabled = false;
            printf("*** CTTY Mode Enabled ***\nPress 'C' again to return to keyboard mode.\n\n");
        } else {
            video_enabled = true;
        }
    } else if (c == 'R') {
        printf("\033[2JReseting cpu\n");
        gpio_put(INTR_PIN, 0);
        reset_cpu();
    } else if (c == 'B') {
        reset_usb_boot(0, 0);
    } else if (c == 'M') {
        printf("\nEnter base address (hex): ");
        uint32_t base = 0;
        while (1) {
            int k = getchar_timeout_us(0);
            if (k == PICO_ERROR_TIMEOUT) continue;
            if (k == '\r' || k == '\n') break;
            if ((k >= '0' && k <= '9') || (k >= 'a' && k <= 'f') || (k >= 'A' && k <= 'F')) {
                k = (k >= 'a') ? k - 'a' + 10 : (k >= 'A') ? k - 'A' + 10 : k - '0';
                base = (base << 4) | k;
                printf("%X", k);
            }
        }
        printf("\nMemory dump from %04X:\n", base);
        for (int i = 0; i < 0x200 && i + base < RAM_SIZE; i += 16) {
            printf("%04X: ", base + i);
            for (int j = 0; j < 16; j++) printf("%02X ", RAM[base + i + j]);
            printf(" | ");
            for (int j = 0; j < 16; j++) {
                uint8_t value = RAM[base + i + j];
                printf("%c", (value >= 32 && value < 127) ? value : '.');
            }
            printf("\n");
            int k = getchar_timeout_us(0);
            if (k != PICO_ERROR_TIMEOUT && (k == 'M' || k == 'R' || k == 'B' || k == 'V')) break;
        }
    } else if (c == 'V') {
        printf("\nEnter base address (hex): ");
        uint32_t base = 0;
        while (1) {
            int k = getchar_timeout_us(0);
            if (k == PICO_ERROR_TIMEOUT) continue;
            if (k == '\r' || k == '\n') break;
            if ((k >= '0' && k <= '9') || (k >= 'a' && k <= 'f') || (k >= 'A' && k <= 'F')) {
                k = (k >= 'a') ? k - 'a' + 10 : (k >= 'A') ? k - 'A' + 10 : k - '0';
                base = (base << 4) | k;
                printf("%X", k);
            }
        }
        printf("\nVideo dump from %04X:\n", base);
        for (int i = 0; i < 0x200 && i + base < RAM_SIZE; i += 16) {
            printf("%04X: ", base + i);
            for (int j = 0; j < 16; j++) printf("%02X ", VIDEORAM[base + i + j]);
            printf(" | ");
            for (int j = 0; j < 16; j++) {
                uint8_t value = VIDEORAM[base + i + j];
                printf("%c", (value >= 32 && value < 127) ? value : '.');
            }
            printf("\n");
            int k = getchar_timeout_us(0);
            if (k != PICO_ERROR_TIMEOUT && (k == 'M' || k == 'R' || k == 'B' || k == 'V')) break;
        }
    } else {
        // Обычный ввод
        if (ctty_mode) {
            uart.rbr = (uint8_t) c;
            uart.data_ready = true;
        } else {
            if (c == 0x1B) {
                ansi_state = ANSI_STATE_ESC;
                ansi_buf_len = 0;
            } else {
                send_ascii_as_scancode(c);
            }
        }
    }
}

#else   /* SERIAL_CONSOLE == 0 */

/* stdio still comes up — printf over UART0 is how a headless bring-up
 * reports itself — but nothing mirrors the screen or injects keystrokes. */
static inline void console_init(void) {
    stdio_init_all();
    setvbuf(stdout, NULL, _IONBF, 0);
}
#define console_task(...)

#endif
