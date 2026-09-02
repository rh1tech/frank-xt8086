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
bool handleScancode(uint8_t ps2scancode);

// === Мышь (Microsoft Serial Mouse protocol) ===
void mouse_init(void);
// Автоматическая отправка данных через COM1 при получении HID mouse reports

// Проверка подключения USB мыши (для UART идентификации)
bool is_usb_mouse_connected(void);
