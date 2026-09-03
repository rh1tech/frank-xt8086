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

#include <hardware/structs/qmi.h>
#include <pico/multicore.h>

#include "state.h"
#include "graphics.h"
#include "psram.h"
#include "cpu_probe.h"
#include "screens.h"
#include "osd.h"
#include "ui_gfx.h"

// 8086 related libs
#include "i8237.h"
#include "i8259.h"
#include "i8253.h"
#include "uart16550.h"
#include "mc146818.h"
#include "adlib.h"
#include "redirector.h"
#include "fslock.h"
#include "audio.h"

#include "ff.h"
#include "f_util.h"
#include "console.h"
#include "memory.h"
#include "vgacard.h"
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

// The open image files. File-scope rather than main()'s locals because
// the drive menu can reopen them long after main() has moved on.
static FIL floppy_files[2];
static FIL hdd_file;
static bool sd_mounted;

/*
 * Open whatever settings currently name, closing whatever was open.
 *
 * Called once at boot and again whenever the drive menu changes
 * something. Reopening rather than patching is what makes a swap take
 * effect immediately: the FDC reads fdd_media_mask on every transfer and
 * the IDE model follows ide.disk_image, so both see the new state on
 * their next access with nothing else to notify.
 */
/*
 * Put the DOS-side tools on the card, next to the disk images.
 *
 * Note what this does *not* do: DOS cannot see them here. The guest only
 * ever sees the contents of the .img files, and /XT is the host
 * filesystem those images live in. Getting a file into DOS means writing
 * it inside an image, which would mean mounting that image as a second
 * FatFs volume and writing into someone's disk -- and a firmware that
 * silently modifies a disk image on every boot is a firmware that will
 * eventually corrupt one. See the README for how to install them.
 *
 * They are written here anyway because it is the one place guaranteed to
 * hold the versions this firmware was built against, so whatever route
 * they take into the guest, they cannot fall out of step with the CMOS
 * emulation they talk to.
 *
 * Rewritten every boot rather than only when missing, for the same
 * reason. Together they are under a kilobyte.
 */
void media_reload(void) {
    /*
     * Held across the whole function, not per call: this closes and
     * reopens the very FIL objects core 1 reads the hard disk through, so
     * a half-swapped set is not merely inconsistent, it is a use of a
     * closed file from an interrupt.
     */
    FS_LOCK();
    if (fdd_media_mask & 1u) f_close(&floppy_files[0]);
    if (fdd_media_mask & 2u) f_close(&floppy_files[1]);
    if (ide.disk_image) f_close(ide.disk_image);
    fdd_media_mask = 0;
    ide.disk_image = nullptr;

    if (!sd_mounted) {
        // No card: drive A: still answers, from the built-in bootOS in
        // flash. See chipset/i8272.h.
        printf("[media] no card; drive A: is the built-in bootOS\n");
        FS_UNLOCK();
        return;
    }

    if (settings.fda[0]) {
        if (FR_OK == f_open(&floppy_files[0], settings.fda, FA_READ | FA_WRITE))
            fdd_media_mask |= 1u << 0;
        else
            printf("[media] A: %s not found\n", settings.fda);
    }
    if (settings.fdb[0]) {
        if (FR_OK == f_open(&floppy_files[1], settings.fdb, FA_READ | FA_WRITE))
            fdd_media_mask |= 1u << 1;
        else
            printf("[media] B: %s not found\n", settings.fdb);
    }
    if (settings.hdd[0]) {
        if (FR_OK == f_open(&hdd_file, settings.hdd, FA_READ | FA_WRITE))
            ide.disk_image = &hdd_file;
        else
            printf("[media] C: %s not found\n", settings.hdd);
    }

    FS_UNLOCK();

    printf("[media] A:%s B:%s C:%s\n",
           (fdd_media_mask & 1) ? settings.fda : "-",
           (fdd_media_mask & 2) ? settings.fdb : "-",
           ide.disk_image ? settings.hdd : "-");
}

/*
 * Everything settings decides once, at boot, rather than every frame.
 *
 * Most of SETUP takes effect on its own: the mode a game gets is read
 * from settings.vga/hercules/tandy_enabled live, every time the video
 * path asks. This function exists for the handful of fields that are not
 * read live, because what they decide has to be settled before anything
 * downstream can be built on it -- the BIOS counts memory once, at its
 * own boot, against whatever ram_limit says at that moment; sound was
 * turned on or off once, at audio_init() time.
 *
 * Called once during the normal boot sequence, and again from
 * osd_settings_menu() in ui/osd.c after a live change, right before it
 * resets the 8086 -- so the boot that follows sees this run's numbers
 * rather than the previous session's.
 */
void apply_boot_time_settings(void) {
    /*
     * The three cards the rest of the firmware actually reads, derived
     * from the one settings.video_card SETUP now edits. A real machine
     * has one card, not some combination of these three, and independent
     * toggles let one be left on that had no business being on with
     * another, or all three off in the one combination that meant plain
     * CGA -- so this is the only place any of tandy_enabled/hercules/vga
     * is assigned.
     *
     * tandy_enabled is not its own choice: a Tandy is a superset of a
     * CGA, so it comes out true whenever the card is not Hercules or
     * VGA, on the same reasoning SETUP's own menu collapses the two into
     * one row -- there is nothing plain CGA offers that a Tandy does
     * not, so there is nothing to lose by always taking the superset.
     */
    settings.hercules       = settings.video_card == VIDEO_CARD_HERCULES;
    settings.vga            = settings.video_card == VIDEO_CARD_VGA;
    settings.tandy_enabled  = !settings.hercules && !settings.vga;

    audio_set_enabled(settings.sound);

    /*
     * VGA takes precedence: its window at 0xA0000 is below Hercules', so
     * claiming it covers both. That leaves the 640K a standard PC has.
     */
    ram_limit = settings.vga       ? 0xA0000u
              : settings.hercules  ? 0xB0000u
                                   : RAM_SIZE;

    // A live change may have edited a drive path as well as a video
    // setting, and there is no cheaper way to tell than to just redo it.
    media_reload();
}
FATFS fs;

// How long the splash is held before booting on its own. Long enough to
// read the hardware report, short enough that a headless board is not
// visibly stuck. Doubles as SETUP's "is anyone there" window.
#define SPLASH_HOLD_MS 4000u

// Sample rate for the synthesised audio. 44.1 kHz because the OPL2's own
// output is band-limited well below it and core 0 has the headroom at
// 504 MHz; drop it to 22050 first if anything starts missing frames.
#define AUDIO_RATE_HZ 44100u

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
    const uint32_t cpu_freq = cpu_frequency_khz();

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
/*
 * Every keystroke passes through here on its way to the 8086 -- which
 * makes it the one place a host hot key can be taken out of the stream
 * before the guest ever sees it.
 *
 * Win+F11 opens the drive menu; Win+F12 opens SETUP live. Win is tracked
 * at the HID layer, in drivers/usbhid/hid_app.c, because it is never
 * translated to an XT scancode at all -- there is no key for it on a
 * real XT keyboard, so is_win_down() is the only trace it leaves.
 *
 * That is also why this chord needed no modifier tracking of its own,
 * unlike the Ctrl+Alt+F1 it replaces: Ctrl and Alt are real keys the
 * guest has to be told about the moment they go down, which is what made
 * closing that menu have to fake their releases on the way out. Win
 * never reaches the guest, so there is nothing to fake here.
 */
volatile bool osd_requested;
volatile bool settings_requested;

bool handleScancode(const uint8_t ps2scancode) {
    if (ps2scancode == 0x00) return false; // Ignore unknown keys

    if (is_win_down() && (ps2scancode == SC_F11 || ps2scancode == SC_F12)) {
        // Swallowed: the guest never learns either key was pressed,
        // which is the point -- otherwise DOS would act on it too.
        if (ps2scancode == SC_F11) osd_requested = true;
        else                       settings_requested = true;
        return true;
    }

    current_scancode = ps2scancode;

    // While a menu is up the keystrokes are its, so the guest is not
    // told about them. It sees a keyboard nobody touched.
    if (!osd_active()) i8259_interrupt(1);   // IRQ1
    return true;
}

[[noreturn]] int main() {
    // IMPORTANT! Dont remove, hack to create .flashdata section for linker
    extern const uint32_t PICO_CLOCK_SPEED_MHZ;
    assert(PICO_CLOCK_SPEED_MHZ == PICO_CLOCK_SPEED);
    /*
     * Hold the speaker quiet before anything else.
     *
     * A reset leaves every pin an input, so GP46 floats until audio_init
     * claims it -- and it is the input of an amplifier, which makes noise
     * out of whatever it picks up. That is the hiss on reboot. Driving it
     * low costs two instructions and covers the whole of startup, which
     * is otherwise the better part of a second of PSRAM, video and card
     * initialisation with nothing holding the pin.
     */
    gpio_init(BEEPER_PIN);
    gpio_set_dir(BEEPER_PIN, GPIO_OUT);
    gpio_put(BEEPER_PIN, 0);

    vreg_disable_voltage_limit();
    vreg_set_voltage(VREG_VOLTAGE_1_65);
    busy_wait_at_least_cycles((SYS_CLK_VREG_VOLTAGE_AUTO_ADJUST_DELAY_US * (uint64_t) XOSC_HZ) / 1000000);

    qmi_hw->m[0].timing = 0x60007305; // 5x FLASH divisor

    set_sys_clock_hz(PICO_CLOCK_SPEED_MHZ, true);

    // Before anything can reach the filesystem, and before core 1 exists.
    fs_lock_init();

    // The console first, always. stdio is UART0 on J1 and needs nothing
    // more than its pins, so it costs microseconds and it means every
    // later step has somewhere to report a failure — including a
    // psram_init() that hangs, which is the one thing here that can.
    console_init();
    printf("[xt8086] %s  %s  clk %u MHz\n",
           FIRMWARE_NAME, FIRMWARE_VERSION, (unsigned)(PICO_CLOCK_SPEED / MHZ));

    psram_init(PSRAM_CS_PIN, PSRAM_FREQ_HZ);

    // The CMOS clock, before core 1 exists: cmos_init() talks I2C, and
    // once the 8086 is running nothing on this path may.
    cmos_init();

    // The OPL2 allocates, so it also belongs before core 1 starts. The
    // DAC likewise claims DMA channels and takes over GP46, which the
    // guest can start writing to the moment the CPU is out of reset.
    adlib_init(AUDIO_RATE_HZ);
    audio_init();

    // The host stack going up is worth one line: it separates "no
    // keyboard plugged in" from "host mode never started", and those two
    // look identical from the far side of a USB socket.
    if (!tusb_init()) printf("[usb] tusb_init failed -- no host stack\n");
    keyboard_init();
    mouse_init();  // Microsoft Serial Mouse over the emulated COM1

    // Video before storage. The prototype mounted the SD card first and
    // dropped to BOOTSEL if it failed, which meant the single most common
    // bring-up fault — no card, or a card with no images on it — produced
    // a dark screen and a board that had vanished off the USB bus. Now the
    // screen is up before anything can go wrong, so a failure is something
    // you can read.
    graphics_init();

    // The EGA/VGA card. Idle until something programs it.
    vgacard_init();
    for (int i = 0; i < 16; i++) {
        graphics_set_palette(i, cga_palette[i]);
    }
    mc6845_init_text_mode();
    graphics_set_mode(TEXTMODE_80x25_COLOR);
    sleep_ms(100);  // let the VGA driver's DMA chain come up

    const bool sd_ok = (FR_OK == f_mount(&fs, "", 1));
    sd_mounted = sd_ok;
    if (!sd_ok) printf("[xt8086] no SD card, or the card could not be mounted\n");

    // Settings live on the card; without one, the defaults stand.
    if (sd_ok) load_settings();

    // Is there actually a CPU in the socket?
    //
    // Before the splash, because the splash reports the answer, and
    // before core 1, because the probe drives the same bus and clock that
    // core 1 is about to take over. Ten milliseconds at the configured
    // speed is many times what an 8086 needs to reach its first fetch.
    const uint32_t cpu_khz = cpu_frequency_khz();
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
    printf("[boot] SETUP %s\n", wants_setup ? "requested" : "not requested");
    if (wants_setup) setup_menu();

    // After SETUP, so a change made there takes effect on this boot
    // rather than the next one.
    apply_boot_time_settings();

    // A short two-note chime, so "it booted" is audible from across the
    // bench without watching the screen. Through the mixer now, like
    // everything else that makes a sound.
    //
    // BEEPER_SWEEP replaces it with the prototype's 500 Hz-5 kHz ramp,
    // which existed to ear-pick the transducer's resonance peak. That is
    // a measurement, not a boot sound: it holds the boot for a second and
    // a half and only ever needs running once per new buzzer part.
#ifdef BEEPER_SWEEP
    for (uint32_t f = 500; f <= 5000; f += 250) audio_beep(f, 80);
#else
    audio_beep(1000, 60);
    audio_beep(1500, 90);
#endif

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
                    case DMA_SOURCE_FILE_READ: {
                        FS_LOCK();
                        f_lseek(&floppy_files[channel->file_index], channel->data_offset);
                        f_read(&floppy_files[channel->file_index], &RAM[dest_addr], size, &br);
                        FS_UNLOCK();
                    }
                        // printf("Read %x size %x offset %x \n", br, size, channel->data_offset);
                        break;
                    case DMA_SOURCE_FILE_WRITE: {
                        FS_LOCK();
                        f_lseek(&floppy_files[channel->file_index], channel->data_offset);
                        f_write(&floppy_files[channel->file_index], &RAM[dest_addr], size, &br);
                        FS_UNLOCK();
                    }
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

        // Audio, every pass rather than on the frame tick: a block is
        // 5.8 ms and the frame tick is 16.6 ms apart, so refilling there
        // would underrun twice out of every three blocks.
        audio_task();
        redirector_task();   // core 0: the guest is spinning while this runs

        if (osd_requested) {
            osd_requested = false;
            osd_drive_menu();
        }

        if (settings_requested) {
            settings_requested = false;
            osd_settings_menu();   // reboots the 8086 if anything was saved
        }

        // Проверка состояния видеоадаптера и обработка клавиатуры
        if (absolute_time_diff_us(next_frame, get_absolute_time()) >= 0) {
            keyboard_tick();
            cmos_tick();   // core 0 only: this is the half that does I2C

            next_frame = delayed_by_us(next_frame, 16666);
            mc6845.cursor_blink_state = frame_counter++ >> 4 & 1;

            /*
             * Re-read the card once a frame. The start address moves as
             * software scrolls, so this cannot be done only on a mode
             * change: a picture that scrolls would sit still.
             */
            /*
             * Built aside and published whole.
             *
             * vgacard_get_frame() begins by zeroing the struct it is
             * given, and the scanline handler is an interrupt on this
             * same core reading that struct. Handing it vga_frame
             * directly opened a window, once every frame, in which the
             * palette was all zeroes.
             *
             * That is not a cosmetic race. The palette bytes carry the
             * sync bits -- pack_colour() ors in 0xC0 to hold HSYNC and
             * VSYNC inactive -- so a scanline rendered from a zeroed
             * palette asserts both on every pixel. The monitor sees
             * sync where the picture should be, drops the mode, and
             * re-locks when the next good frame arrives: the display
             * cutting in and out in EGA modes while text and CGA, which
             * do not read this palette, stayed rock solid.
             *
             * The copy is not atomic either, but every byte in it is a
             * valid entry, so the worst a torn read can do is mix two
             * good frames.
             */
            vgacard_frame_t next_frame;
            vgacard_get_frame(&next_frame);
            /*
             * Everything except the three the scanline handler owns.
             *
             * start_addr, panning and line_compare are snapped once a
             * frame in the interrupt, during blanking, so that every line
             * is drawn from one position. Assigning the whole struct here
             * put vgacard_get_frame()'s unsnapped copies straight back
             * over them at whatever moment this loop happened to run --
             * one frame drawn from somewhere else, which is a jolt in a
             * scrolling picture.
             */
            vga_frame.submode     = next_frame.submode;
            vga_frame.width       = next_frame.width;
            vga_frame.height      = next_frame.height;
            vga_frame.line_offset = next_frame.line_offset;
            memcpy(vga_frame.palette, next_frame.palette,
                   sizeof vga_frame.palette);

            /*
             * In text mode the card is the one being programmed, so the
             * 6845 model follows it rather than the other way round.
             *
             * The video BIOS at 0xC0000 drives a VGA CRTC, whose
             * registers do not mean what a 6845's do -- eighty columns
             * reads as 79, and the row count is not held in any register
             * a 6845 has. The text renderer works in 6845 terms, so the
             * shape is translated here once a frame rather than teaching
             * every renderer two dialects.
             */
            /*
             * Set every frame, both ways -- not just to true.
             *
             * This used to be assigned only when geometry read back
             * successfully, which means it was never assigned back to
             * false: turn VGA off in SETUP and the flag from the last
             * frame VGA was on stayed true, since nothing about turning
             * the card off touches RAM that reset_cpu() does not reach
             * either -- that pin restarts the 8086, not this firmware's
             * own variables. The renderer went on taking text from the
             * card's now-stale memory while every write, correctly, was
             * going to the plain CGA buffer instead: the right shape,
             * because geometry still came from wherever this flag
             * pointed, and every glyph wrong, because that stopped being
             * where DOS was writing.
             */
            extern bool vga_text_from_card;
            vga_text_from_card = false;

            if (settings.vga && vga_frame.submode == 0) {
                vgacard_text_t t;
                if (vgacard_text_geometry(&t)) {
                    vga_text_from_card = true;
                    mc6845.r.h_displayed       = t.columns;
                    mc6845.r.v_displayed       = t.rows;
                    mc6845.r.max_scanline_addr = (uint8_t)(t.char_height - 1u);
                    mc6845.r.cursor_start      = t.cursor_start;
                    mc6845.r.cursor_end        = t.cursor_end;
                    mc6845.vram_offset         = (uint32_t)t.start_addr << 1;
                    mc6845.cursor_x            = t.cursor_addr % t.columns;
                    mc6845.cursor_y            = t.cursor_addr / t.columns;
                    // Re-select the video mode when the shape changes:
                    // nothing else will, because the video BIOS does not
                    // touch the CGA registers that normally trigger it.
                    static uint8_t last_cols;
                    if (t.columns != last_cols) { last_cols = t.columns; cga.updated = true; }
                }
            }

            /*
             * A change of card mode has to drive the selection below, and
             * nothing else would: that block runs when a CGA port is
             * written, and programming the EGA touches none. Without this
             * the card sat correctly in mode 0Dh while the display stayed
             * in text.
             */
            static int last_submode;
            if (vga_frame.submode != last_submode) {
                last_submode = vga_frame.submode;
                cga.updated = true;
            }

            console_task(videomode);
        }

        if (cga.updated) {
                /*
                 * Hercules first, because it is not a CGA mode at all and
                 * none of the CGA register logic below applies to it.
                 *
                 * Both conditions matter. Bit 1 of the mode register asks
                 * for graphics, and bit 0 of the configuration switch is
                 * the card's own permission for graphics to exist -- an
                 * MDA program that never touches 0x3BF must not be
                 * mistaken for a Hercules one.
                 *
                 * The memory has already been claimed by then. The
                 * framebuffer is at 0xB0000, inside the 736K this machine
                 * normally offers, so the SETUP option lowers the ceiling
                 * before the BIOS counts memory. Doing it here instead
                 * would take 32K away from a guest that had already been
                 * told it had them.
                 */
                /*
                 * A VGA mode, if the option ROM asked for one, outranks
                 * everything below: nothing about the CGA registers
                 * describes it, and the card the guest thinks it is
                 * driving is not the one the rest of this decides.
                 */
                /*
                 * cga.vga_mode_request, and not merely the card's own
                 * submode, because the card stays programmed for the last
                 * graphics mode until someone programs it otherwise.
                 *
                 * Port 0x3DC carries the option ROM's request, and a zero
                 * written there is how it hands the display back on any
                 * mode set that is not one of ours. Reading only the
                 * submode ignored that: after a warm reset the CPU
                 * restarted, the BIOS set its text mode, and the screen
                 * carried on being drawn from the EGA planes at 0xA0000
                 * while DOS wrote text to 0xB8000. The machine looked
                 * dead when it was running perfectly.
                 */
                /*
                 * The card's own state, which is authoritative again now
                 * that a real video BIOS programs it. The extra test on
                 * cga.vga_mode_request existed because the only thing
                 * that set a graphics mode was our option ROM writing
                 * port 0x3DC, and the card stayed programmed for it
                 * afterwards; the video BIOS puts the card back to text
                 * itself, so the submode alone is the truth.
                 */
                if (settings.vga && vga_frame.submode) {
                    // The card knows what it was programmed for; width and
                    // height come from its own CRTC rather than the mode
                    // number, so a program that adjusts the geometry gets
                    // what it asked for.
                    if (vga_frame.submode == 3)
                        videomode = VGA_320x200x256;
                    else if (vga_frame.width <= 320)
                        videomode = EGA_320x200x16x4;
                    else if (vga_frame.height > 200)
                        videomode = EGA_640x350x16x4;
                    else
                        videomode = EGA_640x200x16x4;

                    if (videomode != old_videomode) {
                        printf("Videomode %i\n", videomode);
                        graphics_set_mode(videomode);
                        old_videomode = videomode;
                    }
                    cga.updated = false;
                    continue;
                }

                const bool herc_wanted = cga.herc_selected &&
                                         (cga.herc_config & 1u) &&
                                         (cga.herc_mode & 0b10);
                const bool herc = settings.hercules && herc_wanted;

                /*
                 * Say so rather than showing nothing.
                 *
                 * With the card switched off its framebuffer at 0xB0000
                 * is ordinary memory, so software asking for Hercules
                 * draws a perfectly good picture into RAM and the screen
                 * stays black. That is indistinguishable from a hang
                 * unless someone says which of the two it is.
                 */
                static bool herc_warned;
                if (herc_wanted && !settings.hercules && !herc_warned) {
                    herc_warned = true;
                    printf("[video] software asked for Hercules graphics, but the card "
                           "is off in SETUP; it is drawing into memory, not video\n");
                }

                if (herc) {
                    videomode = HERC_720x348x2;

                    // One bit a pixel and no palette register anywhere on
                    // the card: a Hercules is on or off, and the colour
                    // was whatever the phosphor happened to be.
                    graphics_set_bgcolor(0);
                    graphics_set_palette(0, cga_palette[0]);
                    graphics_set_palette(1, cga_palette[15]);
                } else if (unlikely(cga.port3D8 & 0b10)) {
                    // Bit 1: Graphics/Text Select
                    if (unlikely(cga.port3D8 & 0b10000)) {
                        {
                            /*
                             * Mode 6 on a composite monitor is not black
                             * and white: the pixel clock is four times the
                             * NTSC subcarrier, so groups of four pixels
                             * come out as colour.
                             *
                             * Bit 2 of the mode register is how software
                             * asks. It disables the colour burst, the BIOS
                             * sets it for mode 6, and a program that wants
                             * artifact colour clears it again -- Planet X3
                             * writes 0x1A where the BIOS left 0x1E. So the
                             * default follows the bit rather than a menu
                             * option nobody knew to turn on.
                             */
                            const bool burst_on = !(cga.port3D8 & 4);
                            const bool composite =
                                    settings.composite == CGA_MONITOR_COMPOSITE ||
                                    (settings.composite == CGA_MONITOR_AUTO && burst_on);

                            videomode = composite ? COMPOSITE_160x200x16
                                                  : CGA_640x200x2;
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
                    /*
                     * How wide the screen is comes from the CRTC, not
                     * from bit 0 of port 0x3D8. That bit is a CGA mode
                     * register and the video BIOS never writes it, so
                     * reading it left an eighty column screen being drawn
                     * forty columns wide with every pixel doubled. The
                     * CRTC's column count is right for either card.
                     */
                    videomode = mc6845.r.h_displayed > 40
                                    ? TEXTMODE_80x25_COLOR : TEXTMODE_40x25_COLOR;
                    graphics_set_bgcolor(cga.port3D9 & 0xF);
                    for (int i = 0; i < 16; i++) {
                        graphics_set_palette(i, cga_palette[i]);
                    }
                }

                if (unlikely(cga.port3DA_tandy /* == 0x20 */)) {
                    // printf("Tandy hack detected: %i\n", videomode);
                    // videomode = (cga.port3D8 & 0b10000) ? TGA_320x200x16 : TGA_160x200x16;
                    /*
                     * Which of the three Tandy modes, decided from the
                     * CRTC rather than from a video-array bit.
                     *
                     * h_displayed counts displayed characters, and the
                     * Tandy 16-colour modes put four bytes in each: 40
                     * characters is 160 bytes a line and 320 pixels, 80 is
                     * 320 bytes and 640. So the register that has to be
                     * right for the picture to be the right width is also
                     * the one that says which mode it is. The video array
                     * has bits that mean this too, but their meaning
                     * varies across Tandy models and I have no way to
                     * verify a guess; the character count is observable
                     * and cannot be inconsistent with what is on screen.
                     */
                    if (mc6845.r.h_displayed >= 80)
                        videomode = TGA_640x200x16;
                    else if (videomode == CGA_640x200x2 ||
                             videomode == COMPOSITE_160x200x16)
                        videomode = TGA_320x200x16;
                    else
                        videomode = TGA_160x200x16;
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
