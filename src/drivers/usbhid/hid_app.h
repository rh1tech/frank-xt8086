#pragma once

#include <stdint.h>
#include <stdbool.h>

// ========================================
// USB HID Driver API
// Поддержка клавиатуры и мыши через USB
// ========================================

// === Клавиатура ===
void keyboard_init(void);
void keyboard_tick(void);

// External handler for scancodes (implemented in main application)
// The scancodes the host itself has to reason about: the two hotkey
// chords, and the releases osd.c fakes on the way out. In the header
// because both the keyboard path and the menu need them.
#define SC_CTRL_MAKE  0x1Du
#define SC_CTRL_BREAK 0x9Du
#define SC_ALT_MAKE   0x38u
#define SC_ALT_BREAK  0xB8u
#define SC_F1         0x3Bu
#define SC_F11        0x57u
#define SC_F12        0x58u

bool handleScancode(uint8_t ps2scancode);

/*
 * Whether a Windows/GUI key is currently held.
 *
 * A real XT keyboard has no such key and no scancode for one, so it is
 * never translated and never reaches the guest -- there would be nothing
 * for DOS to do with it. It exists here only as a hotkey modifier: Win
 * held with F11 or F12 is how the host reaches the drive menu and the
 * live settings menu, chosen because it is a chord no DOS program of
 * this era binds to anything.
 */
bool is_win_down(void);

/*
 * Put a scancode into the keyboard queue as though a key had produced it.
 *
 * For codes the guest needs but no key will send: see the modifier
 * releases the drive menu has to fake in ui/osd.c.
 */
void keyboard_inject(uint8_t xt_scancode);

// === Мышь (Microsoft Serial Mouse protocol) ===
void mouse_init(void);
// Автоматическая отправка данных через COM1 при получении HID mouse reports

// Проверка подключения USB мыши (для UART идентификации)
bool is_usb_mouse_connected(void);
