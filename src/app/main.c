/*
 * frank-xt8086 — an RP2350B acting as the whole chipset for a real 8086
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-xt8086
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Core 0 runs everything that can afford to be interrupted: DMA, the
 * video-mode follower, the keyboard, the console. Core 1 does nothing but
 * hold the 8086's bus, and it is the PIO and its two interrupt handlers in
 * src/core/cpu_bus.c that actually answer the CPU — core 1 only raises INTR.
 */

#include <hardware/pwm.h>
#include <hardware/structs/qmi.h>
#include <pico/multicore.h>

#include "state.h"
#include "graphics.h"
#include "psram.h"
#include "cpu_probe.h"
#include "screens.h"
#include "ui_gfx.h"

// 8086 related libs
#include "i8237.h"
#include "i8259.h"
#include "i8253.h"
#include "uart16550.h"

#include "ff.h"
#include "f_util.h"
#include "console.h"
#include "setup_menu.h"
extern cga_s cga;
extern mc6845_s mc6845;
extern ide_s ide;

uint8_t videomode = 0;
repeating_timer_t irq0_timer;

uint8_t current_scancode = 0; // 0 = нет данных

// Which floppy drives have an image open on the card, bit per drive. The
// FDC reads this to decide between the SD file and the built-in bootOS
// image; see src/chipset/i8272.h.
uint8_t fdd_media_mask = 0;
pwm_config pwm;
FATFS fs;

// PWM prescaler for the speaker. 127 keeps the counter's base rate low
// enough that a 500 Hz wrap still fits in sixteen bits at 504 MHz, which
// is the bottom of the range the PC speaker ever asks for.
#define BEEPER_CLKDIV 127

// One tone, then silence. Held on the caller's thread, which is fine —
// every use is during bring-up, before core 1 has the bus.
static void beep(const uint slice, const uint32_t pwm_base_hz,
                 const uint32_t freq_hz, const uint32_t ms) {
    const uint16_t wrap = (uint16_t)(pwm_base_hz / freq_hz);
    pwm_set_wrap(slice, wrap);
    pwm_set_gpio_level(BEEPER_PIN, wrap / 2);   // 50 % for full AC swing
    sleep_ms(ms);
}

// How long the splash is held before booting on its own. Long enough to
// read the hardware report, short enough that a headless board is not
// visibly stuck. Doubles as SETUP's "is anyone there" window.
#define SPLASH_HOLD_MS 4000u

static inline void pic_init(void) {
    // Настройка INTR как выход
    gpio_init(INTR_PIN);
    gpio_set_dir(INTR_PIN, GPIO_OUT);
    gpio_put(INTR_PIN, 0); // По умолчанию LOW
}

// ============================================================================
// Core1: Обработка i8086_bus
// ============================================================================

[[noreturn]] void bus_handler_core(void) {
    // Таблица частот: индекс 0 = 1MHz, 1 = 4.75MHz, 2 = 6MHz
    static constexpr uint32_t cpu_frequencies[] = {1000, 4750, 6000};
    const uint32_t cpu_freq = cpu_frequencies[settings.cpu_freq_index];

    start_cpu_clock(cpu_freq); // Start i8086 clock generator
    pic_init(); // Initialize interrupt controller and start Core1 IRQ generator
    cpu_bus_init(); // Initialize bus BEFORE releasing i8086 from reset
    reset_cpu(); // Now i8086 can safely start

#ifdef BUS_TRACE_PIN
    gpio_init(BUS_TRACE_PIN);
    gpio_set_dir(BUS_TRACE_PIN, GPIO_OUT);
    gpio_put(BUS_TRACE_PIN, 1);
#endif

    while (true) {
        // Управление сигналом INTR
        gpio_put(INTR_PIN, i8259_get_pending_irqs());

        __wfe();
        tight_loop_contents();
    }
}


// ============================================================================
// Set scancode and trigger IRQ1 (keyboard interrupt)
// ============================================================================
bool handleScancode(const uint8_t ps2scancode) {
    if (ps2scancode == 0x00) return false; // Ignore unknown keys

    current_scancode = ps2scancode;
    i8259_interrupt(1); // IRQ1 - Keyboard interrupt через i8259
    return true;
}

[[noreturn]] int main() {
    // IMPORTANT! Dont remove, hack to create .flashdata section for linker
    extern const uint32_t PICO_CLOCK_SPEED_MHZ;
    assert(PICO_CLOCK_SPEED_MHZ == PICO_CLOCK_SPEED);
    vreg_disable_voltage_limit();
    vreg_set_voltage(VREG_VOLTAGE_1_65);
    busy_wait_at_least_cycles((SYS_CLK_VREG_VOLTAGE_AUTO_ADJUST_DELAY_US * (uint64_t) XOSC_HZ) / 1000000);

    qmi_hw->m[0].timing = 0x60007305; // 5x FLASH divisor

    set_sys_clock_hz(PICO_CLOCK_SPEED_MHZ, true);

    // The console first, always. stdio is UART0 on J1 and needs nothing
    // more than its pins, so it costs microseconds and it means every
    // later step has somewhere to report a failure — including a
    // psram_init() that hangs, which is the one thing here that can.
    console_init();
    printf("[xt8086] %s  %s  clk %u MHz\n",
           FIRMWARE_NAME, FIRMWARE_VERSION, (unsigned)(PICO_CLOCK_SPEED / MHZ));

    psram_init(PSRAM_CS_PIN, PSRAM_FREQ_HZ);

    tusb_init();
    keyboard_init();
    mouse_init();  // Microsoft Serial Mouse over the emulated COM1

    // Video before storage. The prototype mounted the SD card first and
    // dropped to BOOTSEL if it failed, which meant the single most common
    // bring-up fault — no card, or a card with no images on it — produced
    // a dark screen and a board that had vanished off the USB bus. Now the
    // screen is up before anything can go wrong, so a failure is something
    // you can read.
    graphics_init();
    for (int i = 0; i < 16; i++) {
        graphics_set_palette(i, cga_palette[i]);
    }
    mc6845_init_text_mode();
    graphics_set_mode(TEXTMODE_80x25_COLOR);
    sleep_ms(100);  // let the VGA driver's DMA chain come up

    FIL floppy_files[2];
    FIL hdd_file;

    const bool sd_ok = (FR_OK == f_mount(&fs, "", 1));
    if (!sd_ok) printf("[xt8086] no SD card, or the card could not be mounted\n");

    // Settings live on the card; without one, the defaults stand.
    if (sd_ok) load_settings();

    // Is there actually a CPU in the socket?
    //
    // Before the splash, because the splash reports the answer, and
    // before core 1, because the probe drives the same bus and clock that
    // core 1 is about to take over. Ten milliseconds at the configured
    // speed is many times what an 8086 needs to reach its first fetch.
    static constexpr uint32_t cpu_frequencies[] = { 1000, 4750, 6000 };
    const uint32_t cpu_khz = cpu_frequencies[settings.cpu_freq_index];
    const cpu_probe_result_t cpu = cpu_probe(cpu_khz, 10);

    if (!cpu.present) {
        printf("[xt8086] no bus activity from the 8086\n");
    } else if (!cpu.vector_ok) {
        printf("[xt8086] 8086 fetched %05lx, expected %05x\n",
               (unsigned long)cpu.first_addr, I8086_RESET_VECTOR);
    } else {
        printf("[xt8086] 8086 present, reset vector ok, %lu cycles\n",
               (unsigned long)cpu.cycles);
    }

    const splash_info_t info = {
        .cpu_mhz         = PICO_CLOCK_SPEED / MHZ,
        .psram_mhz       = PSRAM_FREQ_HZ / MHZ,
        .sd_ok           = sd_ok,
        .cpu8086_present = cpu.present && cpu.vector_ok,
        .cpu8086_seen    = cpu.present,
        .cpu8086_addr    = cpu.first_addr,
        .cpu8086_khz     = cpu_khz,
    };

    // Reported, never fatal.
    //
    // This started out stopping the boot on a wrong reset vector, on the
    // grounds that a miswired address bus is worth refusing to run. That
    // was wrong twice over: the probe can misread, and it did — a warm
    // reset leaves the 8086 frozen mid-cycle and its stale bus state was
    // being captured as the first fetch — and a machine that boots fine is
    // then held at a red screen by its own diagnostic. A check that can
    // brick a working board is worse than no check. It goes on the splash
    // and the boot continues.
    // The splash doubles as the window in which an operator can ask for
    // SETUP. No key pressed means nobody is watching, so boot.
    const bool wants_setup = screen_splash(&info, SPLASH_HOLD_MS);

    if (!sd_ok) {
        screen_warning(" No microSD ",
                       "No card, so no disk images and no saved settings.",
                       "Drive A: falls back to the built-in bootOS.", 4000);
    }

    // SETUP, before the disks are opened and before core 1 starts.
    if (wants_setup) setup_menu();

    // Открываем первую дискетку (обязательная) - используем путь из settings
    if (sd_ok && settings.fda[0] != '\0') {
        if (FR_OK != f_open(&floppy_files[0], settings.fda, FA_READ | FA_WRITE)) {
            printf("Floppy image not found: %s\n", settings.fda);
        } else {
            fdd_media_mask |= 1u << 0;
        }
    }

    // Открываем вторую дискетку (опциональная) - используем путь из settings
    if (sd_ok && settings.fdb[0] != '\0') {
        if (FR_OK != f_open(&floppy_files[1], settings.fdb, FA_READ | FA_WRITE)) {
            printf("Warning: Second floppy image (%s) not found, drive B: will be unavailable\n", settings.fdb);
        } else {
            fdd_media_mask |= 1u << 1;
        }
    } else {
        printf("Floppy B: disabled (no image selected)\n");
    }

    // Открываем HDD образ - используем путь из settings
    ide.disk_image = &hdd_file;
    if (sd_ok && settings.hdd[0] != '\0') {
        if (FR_OK != f_open(ide.disk_image, settings.hdd, FA_READ | FA_WRITE)) {
            printf("Warning: HDD image (%s) not found, drive C: will be unavailable\n", settings.hdd);
            ide.disk_image = nullptr;  // Сбрасываем указатель если диск не найден
        }
    } else {
        printf("HDD disabled (no image selected)\n");
        ide.disk_image = nullptr;
    }

    pwm = pwm_get_default_config();
    gpio_set_function(BEEPER_PIN, GPIO_FUNC_PWM);
    pwm_config_set_clkdiv(&pwm, BEEPER_CLKDIV);
    pwm_init(pwm_gpio_to_slice_num(BEEPER_PIN), &pwm, true);

    // A short two-note chime, so "it booted" is audible from across the
    // bench without watching the screen.
    //
    // BEEPER_SWEEP replaces it with the prototype's 500 Hz-5 kHz ramp,
    // which existed to ear-pick the transducer's resonance peak. That is a
    // measurement, not a boot sound: it holds the boot for a second and a
    // half and it only ever needs running once per new buzzer part.
    {
        const uint slice = pwm_gpio_to_slice_num(BEEPER_PIN);
        const uint32_t pwm_base_hz = PICO_CLOCK_SPEED / BEEPER_CLKDIV;
#ifdef BEEPER_SWEEP
        for (uint32_t f = 500; f <= 5000; f += 250) {
            beep(slice, pwm_base_hz, f, 80);
        }
#else
        beep(slice, pwm_base_hz, 1000, 60);
        beep(slice, pwm_base_hz, 1500, 90);
#endif
        pwm_set_gpio_level(BEEPER_PIN, 0);
    }

    // Запуск второго ядра с i8086
    multicore_launch_core1(bus_handler_core);

    absolute_time_t next_frame = get_absolute_time();
    next_frame = delayed_by_us(next_frame, 16666);

    uint32_t frame_counter = 0;
    uint8_t old_videomode = 0;
    while (true) {
        // Обработка DMA
        for (dma_channel_s *channel = dma_channels; channel < dma_channels + DMA_CHANNELS; channel++) {
            if (channel->dreq && !channel->masked) {
                // Вычисляем физический адрес назначения
                const uint32_t dest_addr = channel->page + channel->address;
                const size_t size = (uint32_t) channel->count + 1;

                size_t br;
                switch (channel->data_source_type) {
                    case DMA_SOURCE_MEM_READ: memcpy(&RAM[dest_addr], channel->data_source + channel->data_offset, size);
                        break;
                    case DMA_SOURCE_MEM_WRITE: memcpy((void *) (channel->data_source + channel->data_offset), &RAM[dest_addr], size);
                        break;
                    case DMA_SOURCE_FILE_READ:
                        f_lseek(&floppy_files[channel->file_index], channel->data_offset);
                        f_read(&floppy_files[channel->file_index], &RAM[dest_addr], size, &br);
                        // printf("Read %x size %x offset %x \n", br, size, channel->data_offset);
                        break;
                    case DMA_SOURCE_FILE_WRITE:
                        f_lseek(&floppy_files[channel->file_index], channel->data_offset);
                        f_write(&floppy_files[channel->file_index], &RAM[dest_addr], size, &br);
                        break;
                }

                // Обновляем счётчики
                update_count(channel, size);

                // Генерируем IRQ если назначен (после завершения передачи!)
                if (channel->finished) {
                    channel->dreq = 0;

                    if (channel->irq)
                        i8259_interrupt(channel->irq);
                    // printf("DMA CH%i transfer compete from %x to %x size %x, irq %d\n", dma_channels-channel, channel->data_source, dest_addr, size, channel->irq);
                }
            }
        }

        // Проверка состояния видеоадаптера и обработка клавиатуры
        if (absolute_time_diff_us(next_frame, get_absolute_time()) >= 0) {
            keyboard_tick();

            next_frame = delayed_by_us(next_frame, 16666);
            mc6845.cursor_blink_state = frame_counter++ >> 4 & 1;

            console_task(videomode);
        }

        if (cga.updated) {
                if (unlikely(cga.port3D8 & 0b10)) {
                    // Bit 1: Graphics/Text Select
                    if (unlikely(cga.port3D8 & 0b10000)) {
                        {
                            videomode = CGA_640x200x2;
                            graphics_set_bgcolor(0);
                            graphics_set_palette(0, 0);
                            graphics_set_palette(1, (cga.port3D8 & 4) ? cga_palette[cga.port3D9 & 0xF] : cga_palette[15]);
                        }
                    } else {
                        videomode = CGA_320x200x4;
                        // If colorburst set -- 3rd palette, else from palette register
                        const uint8_t palette = (cga.port3D8 & 4) ? 2 : ((cga.port3D9 >> 5) & 1);
                        const uint8_t intensity = (cga.port3D9 >> 4) & 1;

                        graphics_set_palette(0, cga_palette[cga.port3D9 & 0xF]);
                        graphics_set_bgcolor(cga.port3D9 & 0xF);
                        for (int i = 1; i < 4; i++) {
                            graphics_set_palette(i, cga_palette[cga_gfxpal[palette][intensity][i]]);
                        }
                    }
                } else {
                    videomode = cga.port3D8 & 1 ? TEXTMODE_80x25_COLOR : TEXTMODE_40x25_COLOR;
                    graphics_set_bgcolor(cga.port3D9 & 0xF);
                    for (int i = 0; i < 16; i++) {
                        graphics_set_palette(i, cga_palette[i]);
                    }
                }

                if (unlikely(cga.port3DA_tandy /* == 0x20 */)) {
                    // printf("Tandy hack detected: %i\n", videomode);
                    // videomode = (cga.port3D8 & 0b10000) ? TGA_320x200x16 : TGA_160x200x16;
                    videomode = videomode == CGA_640x200x2 ? TGA_320x200x16 : TGA_160x200x16;
                    // graphics_set_bgcolor(cga.port3D9 & 0xF);
                    for (int i = 0; i < 16; i++) {
                        graphics_set_palette(i, cga_palette[i]);
                    }

                }

                if (videomode != old_videomode) {
                    printf("Videomode %i\n", videomode);
                    graphics_set_mode(videomode);
                    old_videomode = videomode;
                }


                // printf("Port 3D8 %x, port 3D9 %x\n", cga.port3D8, cga.port3D9);
                cga.updated = false;
            }
        //__wfi();
        tight_loop_contents();
    }
}
