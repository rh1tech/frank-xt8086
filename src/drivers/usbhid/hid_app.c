// ========================================
// USB HID Application Layer
// Поддержка клавиатуры и мыши через TinyUSB
//
// Архитектура:
//   - Keyboard: USB HID → XT Scancode → handleScancode() в main app
//   - Mouse: USB HID → Microsoft Serial Mouse protocol → COM1
// ========================================

#include "hid_app.h"
#include <stdio.h>
#include "tusb.h"
#include "class/hid/hid.h"
#include "pico/util/queue.h"
#include "uart16550.h"  // Для отправки данных мыши через COM1
#include "usb_to_xt_scancodes.h"

// ========================================
// === Клавиатура (USB HID → XT Scancode) ===
// ========================================
static hid_keyboard_report_t prev_report = {0, 0, {0}};

// ========================================
// === Мышь (Microsoft Serial Mouse) ===
// ========================================
// Обрабатываем HID mouse reports и конвертируем их напрямую в
// Microsoft Serial Mouse protocol (3 байта через COM1 UART)
//
// Автоопределение мыши:
// 1. tuh_hid_mount_cb() устанавливает флаг usb_mouse_connected = true
// 2. DOS драйвер устанавливает DTR/RTS на COM1
// 3. uart_write() в uart16550.h проверяет is_usb_mouse_connected()
// 4. Если мышь подключена → отправляет идентификатор "M"
// 5. DOS драйвер распознает Microsoft Serial Mouse

static bool usb_mouse_connected = false;

static queue_t keyboard_queue;
void keyboard_init(void) {
    queue_init(&keyboard_queue, sizeof(uint8_t), 32);
}

void mouse_init() {
    // Microsoft Serial Mouse автоинициализация:
    // 1. При установке DTR/RTS драйвером → UART отправит идентификатор "M" (uart16550.h)
    // 2. HID mouse reports → автоматически конвертируются в Serial Mouse packets через COM1
    // 3. DOS драйвер (CTMOUSE, MOUSE.COM) читает данные из COM1 (порт 0x3F8)
    usb_mouse_connected = false;
}

bool is_usb_mouse_connected(void) {
    return usb_mouse_connected;
}

__force_inline static void kbd_add_sequence(const uint8_t *sequence) {
    if (!sequence)
        return;
    while(*sequence) {
        queue_try_add(&keyboard_queue, sequence++);
    }
}

// Конвертирует USB HID keycode в XT scancode и добавляет в очередь
// is_release: 0 = make code (нажатие), 1 = break code (отпускание)
static void kbd_raw_key(int usb_code, int is_release) {
    const uint8_t *sequence = (const uint8_t*)conversion[usb_code].d[is_release];
    kbd_add_sequence(sequence);
}

static void kbd_raw_key_down(int usb_code) {
    kbd_raw_key(usb_code, 0);
}

static void kbd_raw_key_up(int usb_code) {
    kbd_raw_key(usb_code, 1);
}

static inline bool find_key_in_report(hid_keyboard_report_t const* report, uint8_t keycode) {
    for (uint8_t i = 0; i < 6; i++) {
        if (report->keycode[i] == keycode) {
            return true;
        }
    }
    return false;
}

static void process_kbd_report(hid_keyboard_report_t const* r1, hid_keyboard_report_t const* r2,
                               void (*kbd_raw_key_cb)(int code)) {

    for(int bit = 8; bit--;) {
        int weight = 1 << bit;
        if((r1->modifier & weight) && !(r2->modifier & weight)) {
            kbd_raw_key_cb(bit + 0xe0);
        }
    }

    // Process keycodes
    for (int i = 0; i < 6; i++) {
        if (r1->keycode[i]) {
            int keycode = r1->keycode[i];
            if (!find_key_in_report(r2, keycode)) {
                kbd_raw_key_cb(keycode);
            }
        }
    }
}

__force_inline static  void find_pressed_keys(hid_keyboard_report_t const* report) {
    process_kbd_report(report, &prev_report, &kbd_raw_key_down);
}

__force_inline static void find_released_keys(hid_keyboard_report_t const* report) {
    process_kbd_report(&prev_report, report, &kbd_raw_key_up);
}

// ========================================
// === Mouse (Microsoft Serial Mouse) ===
// ========================================

// Обработка HID mouse report и конвертация в Microsoft Serial Mouse protocol
// Формат протокола (3 байта):
//   Byte 0: [0 1 L R Y7 Y6 X7 X6] - sync byte (биты 7,6=0,1; L,R - кнопки; X7-X6,Y7-Y6 - старшие биты координат)
//   Byte 1: [0 0 X5 X4 X3 X2 X1 X0] - младшие 6 бит X смещения
//   Byte 2: [0 0 Y5 Y4 Y3 Y2 Y1 Y0] - младшие 6 бит Y смещения
static void process_mouse_report(uint8_t const* report, uint16_t len) {
    if (len < 3) return; // Минимальный размер HID mouse report

    // ========================================
    // Шаг 1: Парсинг HID mouse report
    // ========================================
    // Стандартный HID mouse report format:
    // Byte 0: buttons (bit0=L, bit1=R, bit2=Middle)
    // Byte 1: X movement (signed 8-bit)
    // Byte 2: Y movement (signed 8-bit)

    const uint8_t buttons = report[0] & 0x03; // Берём только L и R кнопки (Microsoft Serial Mouse = 2-button)
    int8_t dx = (int8_t)report[1];
    int8_t dy = (int8_t)report[2];

    // ========================================
    // Шаг 2: Конвертация в Microsoft Serial Mouse protocol
    // ========================================
    // Microsoft Serial Mouse использует 7-битные координаты со знаком
    // Диапазон: -64..+63 (6 бит + знак в старшем бите синхронизации)

    // Ограничиваем координаты до ±63 (чтобы влезли в 6 бит + знак)
    if (dx > 63) dx = 63;
    if (dx < -64) dx = -64;
    if (dy > 63) dy = 63;
    if (dy < -64) dy = -64;

    // Формируем 3 байта протокола Microsoft Serial Mouse
    uint8_t packet[3];

    // Byte 0: [0 1 L R Y7 Y6 X7 X6]
    //   биты 7,6 = 0,1 (синхронизация)
    //   бит 5 = Left button (1=нажата)
    //   бит 4 = Right button (1=нажата)
    //   биты 3,2 = старшие биты Y (Y7, Y6)
    //   биты 1,0 = старшие биты X (X7, X6)
    packet[0] = 0x40;  // Биты 7,6 = 0,1
    packet[0] |= ((buttons & 0x01) << 5);  // Left button → bit 5
    packet[0] |= ((buttons & 0x02) << 3);  // Right button → bit 4
    packet[0] |= ((dy >> 4) & 0x0C);       // Y7,Y6 → bits 3,2
    packet[0] |= ((dx >> 6) & 0x03);       // X7,X6 → bits 1,0

    // Byte 1: [0 0 X5 X4 X3 X2 X1 X0]
    packet[1] = dx & 0x3F;

    // Byte 2: [0 0 Y5 Y4 Y3 Y2 Y1 Y0]
    packet[2] = dy & 0x3F;

    // ========================================
    // Шаг 3: Отправка через COM1 (UART)
    // ========================================
    uart_write_byte(packet[0]);
    uart_write_byte(packet[1]);
    uart_write_byte(packet[2]);
}

/*
 * Device-level attach and detach.
 *
 * These fire for anything that enumerates, the hub included, which is
 * what makes them worth having on a board where every device is behind
 * one: they separate "nothing is reaching the host controller at all"
 * from "the hub came up but the thing plugged into it did not".
 *
 * Only on plug and unplug, so the cost is nil.
 */
void tuh_mount_cb(uint8_t dev_addr) {
    uint16_t vid = 0, pid = 0;
    tuh_vid_pid_get(dev_addr, &vid, &pid);
    // Address 1 is whatever enumerated first, which on this board is the
    // hub itself; anything higher is a device behind it.
    printf("[usb] device %u attached, VID:PID %04X:%04X%s\n",
           dev_addr, vid, pid, dev_addr > 1 ? " (behind the hub)" : "");
}

void tuh_umount_cb(uint8_t dev_addr) {
    printf("[usb] device %u detached\n", dev_addr);
}

void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance, uint8_t const* desc_report, uint16_t desc_len) {
    (void)desc_len;
    (void)desc_report;

    uint16_t vid, pid;
    tuh_vid_pid_get(dev_addr, &vid, &pid);

    uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);

    static const char *proto[] = { "none (raw)", "keyboard", "mouse" };
    printf("[usb] HID %u.%u mounted, VID:PID %04X:%04X, protocol %s\n",
           dev_addr, instance, vid, pid,
           itf_protocol < 3 ? proto[itf_protocol] : "?");

    if (itf_protocol == HID_ITF_PROTOCOL_MOUSE) {
        usb_mouse_connected = true;
    }

    // A keyboard that reports protocol "none" is in report mode and will
    // send a vendor-specific layout this driver does not parse. Asking for
    // boot protocol makes it send the six-key report process_kbd_report()
    // expects. Silently skipping this is a keyboard that mounts and then
    // does nothing, which is the failure that is hardest to see.
    if (itf_protocol == HID_ITF_PROTOCOL_NONE) {
        printf("[usb]   requesting boot protocol\n");
        tuh_hid_set_protocol(dev_addr, instance, HID_PROTOCOL_BOOT);
    }

    if (!tuh_hid_receive_report(dev_addr, instance)) {
        printf("[usb]   ERROR: could not arm the first report\n");
    }
}

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance, uint8_t const* report, uint16_t len) {
    (void)instance;
    (void)dev_addr;

    uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);

    if (itf_protocol == HID_ITF_PROTOCOL_KEYBOARD) {
        const hid_keyboard_report_t *kbd_report = (const hid_keyboard_report_t*)report;

        find_pressed_keys(kbd_report);
        find_released_keys(kbd_report);
        memcpy(&prev_report, report, sizeof(hid_keyboard_report_t));
    } else if (itf_protocol == HID_ITF_PROTOCOL_MOUSE) {
        process_mouse_report(report, len);
    }

    tuh_hid_receive_report(dev_addr, instance);
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance) {
    printf("[usb] HID %u.%u unmounted\n", dev_addr, instance);
    // Проверяем, была ли отключена мышь
    uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);
    if (itf_protocol == HID_ITF_PROTOCOL_MOUSE) {
        usb_mouse_connected = false;
        // printf("HID Mouse disconnected\n");
    }
}


void keyboard_tick(void) {
    tuh_task();
    uint8_t xt_code;
    if (queue_try_remove(&keyboard_queue, &xt_code)) {
        handleScancode(xt_code);
    }
}
