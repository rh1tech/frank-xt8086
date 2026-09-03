// ========================================
// USB HID Application Layer
// Поддержка клавиатуры и мыши через TinyUSB
//
// Архитектура:
//   - Keyboard: USB HID → XT Scancode → handleScancode() в main app
//   - Mouse: USB HID → Microsoft Serial Mouse protocol → COM1
// ========================================

#include <pico/time.h>

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

extern uint8_t current_scancode;   // defined in app/main.c

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

/*
 * Typematic repeat.
 *
 * A real XT keyboard repeats by itself -- the controller in the keyboard
 * holds the key down and re-sends the make code, and the BIOS just sees
 * more of them. USB HID has no such thing: a held key produces one report
 * and then silence until something changes, so a held key did nothing at
 * all here. The host has to generate it.
 *
 * The IBM defaults: about half a second before the first repeat, then
 * roughly ten a second. Only the most recent key repeats, which is what
 * a real keyboard does too -- pressing a second key takes the repeat away
 * from the first.
 *
 * Only make codes are sent, never breaks. That is exactly the stream a
 * real keyboard produces, so nothing downstream needs to know.
 */
#define TYPEMATIC_DELAY_MS 500u
#define TYPEMATIC_PERIOD_MS 92u   /* ~10.9 characters per second */

static int             repeat_code = -1;
static absolute_time_t repeat_due;

/*
 * Left and right GUI/Windows key, as the modifier scan numbers them:
 * bit 3 and bit 7 of the HID modifier byte, offset by 0xE0 the same way
 * every other modifier is. conversion[] has no entry for either -- there
 * is no XT scancode for a key that never existed on an XT keyboard -- so
 * kbd_raw_key() below is a harmless no-op for them and this is the only
 * effect either one has.
 */
#define USB_LGUI 0xE3
#define USB_RGUI 0xE7

static bool win_down;

bool is_win_down(void) { return win_down; }

static void kbd_raw_key_down(int usb_code) {
    kbd_raw_key(usb_code, 0);

    if (usb_code == USB_LGUI || usb_code == USB_RGUI) win_down = true;

    // Modifiers arrive here as 0xE0..0xE7 from the modifier scan. Shift
    // repeating on its own would be meaningless and would steal the
    // repeat from the key being shifted.
    if (usb_code < 0xE0) {
        repeat_code = usb_code;
        repeat_due  = make_timeout_time_ms(TYPEMATIC_DELAY_MS);
    }
}

static void kbd_raw_key_up(int usb_code) {
    kbd_raw_key(usb_code, 1);

    if (usb_code == USB_LGUI || usb_code == USB_RGUI) win_down = false;

    if (usb_code == repeat_code) repeat_code = -1;
}

// Called from keyboard_tick(), on core 0.
static void typematic_task(void) {
    if (repeat_code < 0) return;
    if (absolute_time_diff_us(get_absolute_time(), repeat_due) > 0) return;

    kbd_raw_key(repeat_code, 0);
    repeat_due = make_timeout_time_ms(TYPEMATIC_PERIOD_MS);
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
/*
 * A USB mouse reports as fast as it samples -- 125 to 1000 times a
 * second, unrelated to any wire speed, because it has no wire. A
 * Microsoft Serial Mouse's whole design assumes the opposite: every
 * packet crosses an actual RS-232 link, so a driver reading it has no
 * way to arrive faster than 1200 baud lets three bytes arrive, roughly
 * 25ms apart. Turning every USB report straight into a packet and
 * writing it through uart_write_byte() one-for-one was asking the guest
 * to keep up with a mouse ten to eighty times faster than the protocol
 * it was written for -- which it cannot, since it is only looking at the
 * port when COM1's own IRQ4 says to, and this firmware fires that IRQ4
 * far faster than 1200 baud ever would. The 16-byte FIFO filled and
 * uart_write_byte() silently dropped the overflow, which is fine for one
 * lost byte and is not fine for the tail of a three-byte packet: the
 * guest's frame sync was left one or two bytes short of a packet whose
 * first byte it had already taken as real, and it read the *next*
 * packet's bytes as if they continued the last one. On a bench capture
 * this showed as two thirds of every packet's bytes dropped and the
 * cursor moving once, correctly, and then not again -- not because
 * nothing more arrived, but because nothing that arrived after was ever
 * going to parse as a valid packet again.
 *
 * The fix is not to send less often -- it is to never present a partial
 * packet as the reason to stop. Every HID report's dx/dy is folded into
 * a running total instead of being turned into a packet on the spot;
 * mouse_flush(), called once a frame from keyboard_tick() rather than
 * once a USB report, is the only place a packet is ever built, and it
 * only builds one when uart_rx_room() proves there is space for all
 * three bytes first. Once built, only what that packet actually carried
 * -- at most +-63 a side -- is subtracted back out of the total, so a
 * flick bigger than one packet can hold is not lost, it is spread over
 * however many frames it takes to drain, the same way a real Microsoft
 * Serial Mouse would spread it over however many packets the wire needs.
 */
static int32_t  mouse_pending_dx, mouse_pending_dy;
static uint8_t  mouse_pending_buttons, mouse_sent_buttons;
static bool     mouse_have_backlog;

/*
 * A real Microsoft Serial Mouse is powered by DTR/RTS -- it has no other
 * power pin, so it sends nothing at all until the driver asserts one.
 * Feeding it movement bytes before that point is not something a real
 * mouse on a real wire could ever do. But that is exactly what turning
 * every HID report straight into queued bytes from boot onward did here:
 * the RX FIFO filled with movement data before CuteMouse ever touched
 * MCR, so by the time it asserted DTR/RTS and read the port expecting
 * 'M' as the very first byte, it found either a stale movement byte
 * ahead of 'M' or a full FIFO that silently dropped 'M' altogether --
 * "device not found" either way, and not because identification itself
 * was wrong, but because it was never the first thing in the pipe.
 */
static inline bool mouse_powered(void) {
    return (uart.mcr & 0x03) != 0; // DTR or RTS asserted
}

static void process_mouse_report(uint8_t const* report, uint16_t len) {
    if (len < 3) return; // Минимальный размер HID mouse report

    if (!mouse_powered()) {
        // Not seen by any driver yet -- nothing to carry forward once one arrives.
        mouse_pending_dx = mouse_pending_dy = 0;
        mouse_have_backlog = false;
        return;
    }

    // Стандартный HID mouse report format:
    // Byte 0: buttons (bit0=L, bit1=R, bit2=Middle)
    // Byte 1: X movement (signed 8-bit)
    // Byte 2: Y movement (signed 8-bit)
    mouse_pending_dx      += (int8_t)report[1];
    mouse_pending_dy      += (int8_t)report[2];
    mouse_pending_buttons  = report[0] & 0x03; // L и R (Microsoft Serial Mouse = 2-button)
    mouse_have_backlog     = true;
}

/*
 * Build and send at most one packet, only if there is room for all
 * three of its bytes and 1200 baud's worth of time has actually passed.
 * Called from keyboard_tick(), so a backlog too big for one packet keeps
 * draining on its own even once the mouse has stopped moving and no new
 * HID report is going to call this again.
 *
 * The room check alone used to be enough of a rate limit: before IRQ4
 * was re-raised for bytes left behind in the FIFO (see uart16550.h), the
 * guest only ever drained one byte per interrupt, so the FIFO stayed
 * backed up and uart_rx_room() rejected most calls on its own. Fixing
 * that drain also removed the accidental throttle -- a guest that empties
 * the FIFO the instant it is written leaves room available on nearly
 * every core0 tick, and a fast mouse has a fresh backlog on nearly every
 * tick too, so packets went out (and IRQ4 fired three times each) far
 * faster than 1200 baud could really carry them. That is plenty of
 * extra traffic through the same highest-priority bus IRQ that answers
 * every 8086 cycle, video reads included, and showed up as flicker on
 * fast moves. A real Microsoft Serial Mouse cannot outrun its own wire;
 * this now can't either.
 */
static void mouse_flush(void) {
    if (!mouse_powered()) {
        // Powered off (or never powered on): drop any backlog rather than
        // unload it as one big jump the moment a driver finally shows up.
        mouse_pending_dx = mouse_pending_dy = 0;
        mouse_have_backlog = false;
        return;
    }
    if (!mouse_have_backlog) return;

    static absolute_time_t next_send_due;
    if (absolute_time_diff_us(get_absolute_time(), next_send_due) > 0) return;

    if (uart_rx_room() < 3) return;   // try again next frame

    // Microsoft Serial Mouse's coordinates are 6 bits and signed, so
    // +-63 a packet; clamp to that and carry whatever is left over.
    int32_t dx = mouse_pending_dx, dy = mouse_pending_dy;
    if (dx > 63) dx = 63; else if (dx < -64) dx = -64;
    if (dy > 63) dy = 63; else if (dy < -64) dy = -64;

    mouse_pending_dx -= dx;
    mouse_pending_dy -= dy;
    mouse_sent_buttons = mouse_pending_buttons;
    mouse_have_backlog = (mouse_pending_dx != 0) || (mouse_pending_dy != 0);

    // Byte 0: [0 1 L R Y7 Y6 X7 X6]
    uint8_t packet[3];
    packet[0]  = 0x40;
    packet[0] |= ((mouse_sent_buttons & 0x01) << 5);  // Left button → bit 5
    packet[0] |= ((mouse_sent_buttons & 0x02) << 3);  // Right button → bit 4
    packet[0] |= ((dy >> 4) & 0x0C);                  // Y7,Y6 → bits 3,2
    packet[0] |= ((dx >> 6) & 0x03);                  // X7,X6 → bits 1,0
    packet[1]  = dx & 0x3F;                           // X5..X0
    packet[2]  = dy & 0x3F;                           // Y5..Y0

    uart_write_byte(packet[0]);
    uart_write_byte(packet[1]);
    uart_write_byte(packet[2]);

    next_send_due = make_timeout_time_ms(25); // ~1200 baud, 3 bytes, 8N1
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

    /*
     * Boot protocol, always -- not just for interfaces that report
     * "none".
     *
     * Both parsers here assume the boot layouts: six-key rollover for the
     * keyboard, and exactly [buttons, dx, dy] for the mouse. In *report*
     * protocol a device may legitimately prefix every report with a
     * report ID, which shifts all three mouse fields by a byte, so
     * process_mouse_report() reads the ID as the button mask and the
     * buttons as X. The pointer then moves on its own and clicks at
     * random, which looks like a driver bug and is not one.
     *
     * Asking unconditionally costs one control transfer at mount and
     * makes the layout the same on every device.
     */
    printf("[usb]   requesting boot protocol\n");
    tuh_hid_set_protocol(dev_addr, instance, HID_PROTOCOL_BOOT);

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
        // The first few reports, so a mouse that moves but does nothing
        // can be told apart from one that is not reporting at all.
        static uint8_t seen;
        if (seen < 3) {
            seen++;
            printf("[usb] mouse report, %u bytes: %02X %02X %02X\n",
                   len, report[0], len > 1 ? report[1] : 0, len > 2 ? report[2] : 0);
        }
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


void keyboard_inject(const uint8_t xt_scancode) {
    queue_try_add(&keyboard_queue, &xt_scancode);
}

void keyboard_tick(void) {
    tuh_task();
    typematic_task();
    mouse_flush();

    /*
     * Hand over the next code only once the guest has taken the last one.
     *
     * Port 0x60 does not self-clear; the BIOS acknowledges by pulsing bit
     * 7 of port 0x61, and that is what zeroes current_scancode. Writing a
     * new code before then simply overwrote the old one, so anything
     * arriving faster than the guest's interrupt handler lost bytes --
     * which for a two-byte sequence like E0 48 means the prefix vanishes
     * and an arrow key turns into whatever the second byte means on its
     * own.
     *
     * The firmware's own menus read current_scancode and zero it
     * themselves, so this drains for them too.
     */
    static absolute_time_t handover_deadline;

    if (current_scancode) {
        /*
         * Waiting, but not forever. If the guest stops acknowledging --
         * early boot before INT 9 is installed, or a wedged handler --
         * an unconditional wait would take the keyboard away entirely,
         * which is a worse failure than the dropped byte this avoids.
         * After the deadline, fall back to the old behaviour and
         * overwrite.
         */
        if (absolute_time_diff_us(get_absolute_time(), handover_deadline) > 0) return;
    }

    uint8_t xt_code;
    if (queue_try_remove(&keyboard_queue, &xt_code)) {
        handleScancode(xt_code);
        handover_deadline = make_timeout_time_ms(50);
    }
}
