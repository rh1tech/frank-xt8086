#include <hardware/dma.h>
#include <hardware/irq.h>
#include <hardware/pio.h>
#include "graphics.h"
#include "state.h"

/*
 * Bit- and byte-reverse, without arm_acle.h.
 *
 * This used __rbit() and __rev() out of <arm_acle.h>. Arm GNU Toolchain
 * 14 resolves __rbit to a builtin and inlines the one instruction; the
 * gcc-arm-none-eabi 13.2 that Debian and Ubuntu ship does not define it
 * for M-profile at all, so the compiler emitted a call to a function that
 * exists in no library and the link failed. Only CI ever saw it, which is
 * exactly what CI is for.
 *
 * RBIT is a single ARMv7-M instruction and the asm is shorter than the
 * conditional include that would otherwise be needed. __builtin_bswap32
 * is portable and compiles to REV.
 */
static inline uint32_t rbit32(const uint32_t v) {
    uint32_t r;
    __asm volatile ("rbit %0, %1" : "=r" (r) : "r" (v));
    return r;
}

// VGA_CSYNC controls 7-pin composite sync vs 8-pin separate H/V sync.
// Defined externally via CMake (-DVGA_CSYNC=1 / 0).
// Treat 0 as undefined so existing #ifdef checks behave correctly.
#if defined(VGA_CSYNC) && (VGA_CSYNC + 0) == 0
#  undef VGA_CSYNC
#endif

// --- external hardware / memory references ---
extern uint8_t port3DA;
extern mc6845_s mc6845;
extern cga_s cga;

// --- PIO program (1 instruction) ---
uint16_t pio_program_VGA_instructions[] = {
    //     .wrap_target
    0x6008, //  0: out    pins, 8
    //     .wrap
};

const struct pio_program pio_program_VGA = {
    .instructions = pio_program_VGA_instructions,
    .length = 1,
    .origin = -1,
};

// --- Display geometry / timing (renamed) ---

constexpr int total_scanlines = 525; // previously N_lines_total
constexpr int visible_scanlines = 480; // previously N_lines_visible
constexpr int vsync_start_line = 490; // previously line_VS_begin
constexpr int vsync_end_line = 491; // previously line_VS_end

// --- timing/template constants and buffer setup ---
constexpr int pixel_clock = 25'175'000; // 25.175Mhz

constexpr int hsync_offset_bytes = 328 * 2; // HS_SHIFT
constexpr int hsync_pulse_width_bytes = 48 * 2; // HS_SIZE // Back porch
constexpr int scanline_bytes = 400 * 2;

#if defined(VGA_CSYNC)
constexpr int VGA_PINS = 7;
constexpr int picture_hshift_bytes = scanline_bytes - hsync_offset_bytes + 12;
#else
constexpr int VGA_PINS = 8;
constexpr int picture_hshift_bytes = scanline_bytes - hsync_offset_bytes;
#endif

// scanline_buffers: [0]=blank, [1]=vsync, [2]=image
enum { VBLANK, VSYNC, IMAGE };
#define SCANLINE_BUFFERS 3
static uint32_t *scanline_buffers[SCANLINE_BUFFERS] __attribute__((aligned(4))) = {0};
static uint32_t scanline_buffer_mem[scanline_bytes * SCANLINE_BUFFERS] __attribute__((aligned(32)));

// --- DMA / PIO channels ---
static int dma_ctrl_channel;
static int dma_data_channel;

static uint16_t palette[256] __attribute__((aligned(4)));
// 2K text palette expanded for fast lookup: 256 * 4 entries
static uint16_t textmode_palette_lut[256 * 4] __attribute__((aligned(4)));

// CGA text-mode palette baked for the on-board R-2R DAC.
// Byte layout (LSB→MSB → GPIO 30..37): B0 B1 G0 G1 R0 R1 H V
// Per-channel intensity: 00=off, 01=⅓, 10=⅔ (~0xAA), 11=full.
// Low-intensity colors 1..7 use the HIGH bit of each pair (10 = ⅔).
// Original baseline used the LOW bit (01 = ⅓) which produced a uniformly
// dim signal — captured CGA blue measured at ~25% instead of ~67%.
constexpr uint16_t textmode_palette[16] __attribute__((aligned(4))) = {
    0b000000 & 0x3f | 0xc0, // 0  Black           B=00 G=00 R=00
    0b000010 & 0x3f | 0xc0, // 1  Blue            B=10 G=00 R=00
    0b001000 & 0x3f | 0xc0, // 2  Green           B=00 G=10 R=00
    0b001010 & 0x3f | 0xc0, // 3  Cyan            B=10 G=10 R=00
    0b100000 & 0x3f | 0xc0, // 4  Red             B=00 G=00 R=10
    0b100010 & 0x3f | 0xc0, // 5  Magenta         B=10 G=00 R=10
    0b100100 & 0x3f | 0xc0, // 6  Brown           B=00 G=01 R=10  (R+½G)
    0b101010 & 0x3f | 0xc0, // 7  Light Gray      B=10 G=10 R=10
    0b010101 & 0x3f | 0xc0, // 8  Dark Gray       B=01 G=01 R=01
    0b000011 & 0x3f | 0xc0, // 9  Light Blue      B=11 G=00 R=00
    0b001100 & 0x3f | 0xc0, // 10 Light Green     B=00 G=11 R=00
    0b001111 & 0x3f | 0xc0, // 11 Light Cyan      B=11 G=11 R=00
    0b110000 & 0x3f | 0xc0, // 12 Light Red       B=00 G=00 R=11
    0b110011 & 0x3f | 0xc0, // 13 Light Magenta   B=11 G=00 R=11
    0b111100 & 0x3f | 0xc0, // 14 Yellow          B=00 G=11 R=11
    0b111111 & 0x3f | 0xc0, // 15 White           B=11 G=11 R=11
};

enum graphics_mode_t graphics_mode;

void __time_critical_func() vga_scanline_dma() {
    static uint32_t scanline = 0; // previously screen_line

    // acknowledge interrupt for control channel
    dma_hw->ints0 = 1u << dma_ctrl_channel;

    // advance scanline/frame counters
    scanline++;
    if (unlikely(scanline == total_scanlines)) {
        scanline = 0;
    }

    // If outside visible area - mark output as finished
    if (unlikely(scanline >= visible_scanlines)) {
        const int is_vsync = (scanline >= vsync_start_line && scanline <= vsync_end_line) ? VSYNC : VBLANK;
        dma_channel_set_read_addr(dma_ctrl_channel, &scanline_buffers[is_vsync], false);
        port3DA = 9;
        return;
    }

    // choose odd/even image buffer pointer
    uint16_t y = scanline;

    // If line index beyond prepared image area — fall back to blank
    if (unlikely(scanline >= mc6845.r.v_displayed * (mc6845.r.max_scanline_addr + 1) * 2)) {
        dma_channel_set_read_addr(dma_ctrl_channel, &scanline_buffers[VBLANK], false);
        port3DA = 1;
        return;
    }
    // Non-interlace: skip odd sublines and fold y
    if (likely((mc6845.r.interlace_mode & 1) == 0)) {
        if (y & 1) {
            port3DA = 1;
            return;
        }
        // is_even = 2;
        y >>= 1; // 200 logical lines
    }
    uint32_t **scanline_output_ptr = &scanline_buffers[IMAGE];
    uint32_t *__restrict scanline_output_32 = *scanline_output_ptr + picture_hshift_bytes / 4;


    // activate output for visible lines
    port3DA = 0;
    switch (graphics_mode) {
        case TEXTMODE_40x25_COLOR:
        case TEXTMODE_40x25_BW: {
            // "слой" символа
            uint8_t char_scanlines = mc6845.r.max_scanline_addr;
            const uint8_t glyph_line = y & char_scanlines;
            char_scanlines++;
            const uint8_t screen_y = y / char_scanlines;

            //указатель откуда начать считывать символы
            const uint32_t *__restrict text_buffer_line = (uint32_t *) &VIDEORAM[mc6845.vram_offset + __fast_mul(screen_y, mc6845.r.h_displayed << 1)];
            __builtin_prefetch(text_buffer_line);

            const bool is_cursor_line_active =
                    (screen_y == mc6845.cursor_y) &&
                    (likely(mc6845.r.cursor_start <= mc6845.r.cursor_end)
                         ? (glyph_line >= mc6845.r.cursor_start && glyph_line <= mc6845.r.cursor_end)
                         : (glyph_line >= mc6845.r.cursor_start || glyph_line <= mc6845.r.cursor_end));

            // Предвычисление позиции курсора
            const int cursor_char_x = is_cursor_line_active ? mc6845.cursor_x : -1;

            for (int char_x = 0; char_x < mc6845.r.h_displayed; char_x += 2) {
                uint32_t dword = *text_buffer_line++;

                // Первый символ из пачки
                uint8_t glyph_pixels = font_8x8[(dword & 0xFF) * char_scanlines + glyph_line];
                dword >>= 8;
                uint8_t color = dword;
                if (unlikely(mc6845.cursor_blink_state && (char_x == cursor_char_x))) {
                    glyph_pixels = 0xff; // Инвертируем все 2-битные пиксели разом
                } else if (unlikely(mc6845.cursor_blink_state && mc6845.text_blinking_mask == 0x7F && color & 0x80)) {
                    glyph_pixels = 0x00;
                }
                const uint16_t *palette_color = &textmode_palette_lut[4 * (color & mc6845.text_blinking_mask)];

                // генерируем 4 блока по 2-битным пикселям (удвоение по горизонтали для 40-колоночного режима)
                for (int k = 0; k < 4; ++k) {
                    const uint16_t palette = palette_color[glyph_pixels & 3];
                    const uint16_t lo = palette & 0xFF;
                    const uint16_t hi = palette >> 8;
                    const uint32_t out32 = (uint32_t) (lo << 8 | lo) | ((uint32_t) (hi << 8 | hi) << 16);
                    *scanline_output_32++ = out32;
                    glyph_pixels >>= 2;
                }

                // Второй символ из пачки
                dword >>= 8;
                glyph_pixels = font_8x8[(dword & 0xFF) * char_scanlines + glyph_line];
                dword >>= 8;
                color = dword;
                if (unlikely(mc6845.cursor_blink_state && ((char_x + 1) == cursor_char_x))) {
                    glyph_pixels = 0xff; // Инвертируем все 2-битные пиксели разом
                } else if (unlikely(mc6845.cursor_blink_state && mc6845.text_blinking_mask == 0x7F && color & 0x80)) {
                    glyph_pixels = 0x00;
                }
                palette_color = &textmode_palette_lut[4 * (color & mc6845.text_blinking_mask)];

                // генерируем 4 блока по 2-битным пикселям (удвоение по горизонтали для 40-колоночного режима)
                for (int k = 0; k < 4; ++k) {
                    const uint16_t palette = palette_color[glyph_pixels & 3];
                    const uint16_t lo = palette & 0xFF;
                    const uint16_t hi = palette >> 8;
                    const uint32_t out32 = (uint32_t) (lo << 8 | lo) | ((uint32_t) (hi << 8 | hi) << 16);
                    *scanline_output_32++ = out32;
                    glyph_pixels >>= 2;
                }
            }
            break;
        }
        case TEXTMODE_80x25_COLOR:
        case TEXTMODE_80x25_BW: {
            // "слой" символа
            uint8_t char_scanlines = mc6845.r.max_scanline_addr;
            const uint8_t glyph_line = y & char_scanlines;
            char_scanlines++;
            const uint8_t screen_y = y / char_scanlines;

            //указатель откуда начать считывать символы
            const uint32_t *__restrict text_buffer_line = (uint32_t *) &VIDEORAM[
                (mc6845.vram_offset + __fast_mul(screen_y, mc6845.r.h_displayed << 1)) & 0x3FFF];
            __builtin_prefetch(text_buffer_line);
            const bool is_cursor_line_active =
                    unlikely(mc6845.cursor_blink_state) &&
                    (screen_y == mc6845.cursor_y) &&
                    (likely(mc6845.r.cursor_start <= mc6845.r.cursor_end)
                         ? (glyph_line >= mc6845.r.cursor_start && glyph_line <= mc6845.r.cursor_end)
                         : (glyph_line >= mc6845.r.cursor_start || glyph_line <= mc6845.r.cursor_end));

            // Предвычисление позиции курсора
            const int cursor_char_x = is_cursor_line_active ? mc6845.cursor_x : -1;

            for (int char_x = 0; char_x < mc6845.r.h_displayed; char_x += 2) {
                uint32_t dword = *text_buffer_line++;

                // Первый символ из пачки
                uint8_t glyph_pixels = font_8x8[(dword & 0xFF) * char_scanlines + glyph_line];
                dword >>= 8;
                uint8_t color = dword;
                if (unlikely(mc6845.cursor_blink_state && (char_x == cursor_char_x))) {
                    glyph_pixels = 0xff; // Инвертируем все 2-битные пиксели разом
                } else if (unlikely(mc6845.cursor_blink_state && mc6845.text_blinking_mask == 0x7F && color & 0x80)) {
                    glyph_pixels = 0x00;
                }
                const uint16_t *palette_color = &textmode_palette_lut[4 * (color & mc6845.text_blinking_mask)];

                // 32-битная запись (2 пикселя за раз)
                *scanline_output_32++ = palette_color[glyph_pixels & 3] | ((uint32_t) palette_color[glyph_pixels >> 2 & 3] << 16);
                *scanline_output_32++ = palette_color[glyph_pixels >> 4 & 3] | ((uint32_t) palette_color[glyph_pixels >> 6] << 16);

                // Второй символ из пачки
                dword >>= 8;
                glyph_pixels = font_8x8[(dword & 0xFF) * char_scanlines + glyph_line];
                dword >>= 8;
                color = dword;

                if (unlikely(mc6845.cursor_blink_state && ((char_x+1) == cursor_char_x))) {
                    glyph_pixels = 0xff; // Инвертируем все 2-битные пиксели разом
                } else if (unlikely(mc6845.cursor_blink_state && mc6845.text_blinking_mask == 0x7F && color & 0x80)) {
                    glyph_pixels = 0x00;
                }

                palette_color = &textmode_palette_lut[4 * (color & mc6845.text_blinking_mask)];

                // 32-битная запись (2 пикселя за раз)
                *scanline_output_32++ = palette_color[glyph_pixels & 3] | ((uint32_t) palette_color[glyph_pixels >> 2 & 3] << 16);
                *scanline_output_32++ = palette_color[glyph_pixels >> 4 & 3] | ((uint32_t) palette_color[glyph_pixels >> 6] << 16);
            }
            break;
        }
        case CGA_320x200x4:
        case CGA_320x200x4_BW: {
            const uint32_t *__restrict cga_row = (uint32_t *) &VIDEORAM[(mc6845.vram_offset + __fast_mul(y >> 1, 80) + ((y & 1) << 13)) & 0x3FFF];
            __builtin_prefetch(cga_row);

            // 2bit buf, 16 pixels at once, 32-bit writes
            for (int x = 20; x--;) {
                const uint32_t dword = *cga_row++; // Fetch 16 pixels from CGA memory

                // младший байт (4 пикселя, 2 записи по 32 бита)
                *scanline_output_32++ = palette[(dword >> 6) & 3] | ((uint32_t) palette[(dword >> 4) & 3] << 16);
                *scanline_output_32++ = palette[(dword >> 2) & 3] | ((uint32_t) palette[dword & 3] << 16);

                // следующий байт (4 пикселя, 2 записи по 32 бита)
                *scanline_output_32++ = palette[(dword >> 14) & 3] | ((uint32_t) palette[(dword >> 12) & 3] << 16);
                *scanline_output_32++ = palette[(dword >> 10) & 3] | ((uint32_t) palette[(dword >> 8) & 3] << 16);

                // следующий байт (4 пикселя, 2 записи по 32 бита)
                *scanline_output_32++ = palette[(dword >> 22) & 3] | ((uint32_t) palette[(dword >> 20) & 3] << 16);
                *scanline_output_32++ = palette[(dword >> 18) & 3] | ((uint32_t) palette[(dword >> 16) & 3] << 16);

                // старший байт (4 пикселя, 2 записи по 32 бита)
                *scanline_output_32++ = palette[(dword >> 30)] | ((uint32_t) palette[(dword >> 28) & 3] << 16);
                *scanline_output_32++ = palette[(dword >> 26) & 3] | ((uint32_t) palette[(dword >> 24) & 3] << 16);
            }
            break;
        }
            default:
        case CGA_640x200x2: {
            const uint32_t *__restrict cga_row = (uint32_t *) &VIDEORAM[__fast_mul(y >> 1, 80) + ((y & 1) << 13) & 0x3FFF];
            __builtin_prefetch(cga_row);
            //1bit buf, 32 pixels at once
            for (int x = 20; x--;) {
                uint32_t dword = rbit32(__builtin_bswap32(*cga_row++)); // Fetch 32 pixels from CGA memory

                // Process 32 pixels in groups of 4 (8 iterations)
                for (int i = 8; i--;) {
                    uint32_t pixel_group = 0;
                    pixel_group |= palette[dword & 1];
                    dword >>= 1;
                    pixel_group |= (uint32_t) palette[dword & 1] << 8;
                    dword >>= 1;
                    pixel_group |= (uint32_t) palette[dword & 1] << 16;
                    dword >>= 1;
                    pixel_group |= (uint32_t) palette[dword & 1] << 24;
                    dword >>= 1;
                    *scanline_output_32++ = pixel_group;
                }
            }
            break;
        }
        case TGA_160x200x16: {
            const uint32_t *__restrict tga_row = (uint32_t *) &VIDEORAM[(__fast_mul(y >> 1, 80) + ((y & 1) << 13)) & 0x3FFF];
            __builtin_prefetch(tga_row);
            for (int x = 20; x--;) {
                const uint32_t dword = *tga_row++; // Fetch 8 pixels from TGA memory

                // Обработка первого байта (2 пикселя, удвоенные по горизонтали = 4 выходных пикселя)
                uint8_t pixel1 = (dword >> 4) & 15;
                uint8_t pixel2 = dword & 15;
                *scanline_output_32++ = palette[pixel1] << 16 | palette[pixel1];
                *scanline_output_32++ = palette[pixel2] << 16 | palette[pixel2];

                // Обработка второго байта
                pixel1 = (dword >> 12) & 15;
                pixel2 = (dword >> 8) & 15;
                *scanline_output_32++ = palette[pixel1] << 16 | palette[pixel1];
                *scanline_output_32++ = palette[pixel2] << 16 | palette[pixel2];

                // Обработка третьего байта
                pixel1 = (dword >> 20) & 15;
                pixel2 = (dword >> 16) & 15;
                *scanline_output_32++ = palette[pixel1] << 16 | palette[pixel1];
                *scanline_output_32++ = palette[pixel2] << 16 | palette[pixel2];

                // Обработка четвертого байта
                pixel1 = (dword >> 28);
                pixel2 = (dword >> 24) & 15;
                *scanline_output_32++ = palette[pixel1] << 16 | palette[pixel1];
                *scanline_output_32++ = palette[pixel2] << 16 | palette[pixel2];
            }
            break;
        }
        case TGA_320x200x16: {
            //4bit buf, 32-bit reads
            const uint32_t *__restrict tga_row = (uint32_t *) &VIDEORAM[(__fast_mul(y >> 2, 160) + ((y & 3) << 13)) & 0x7FFF];
            __builtin_prefetch(tga_row);
            for (int x = 40; x--;) {
                const uint32_t dword = *tga_row++; // Fetch 8 pixels from TGA memory

                // Обработка 4 байтов (8 пикселей)
                *scanline_output_32++ = palette[dword & 15] << 16 | palette[(dword >> 4) & 15];
                *scanline_output_32++ = palette[(dword >> 8) & 15] << 16 | palette[(dword >> 12) & 15];
                *scanline_output_32++ = palette[(dword >> 16) & 15] << 16 | palette[(dword >> 20) & 15];
                *scanline_output_32++ = palette[(dword >> 24) & 15] << 16 | palette[(dword >> 28)];
            }
            break;
        }
    }

    dma_channel_set_read_addr(dma_ctrl_channel, scanline_output_ptr, false);
    port3DA = 1; // no more data shown
}

void graphics_set_bgcolor(const uint32_t color888) {
    uint32_t *scanline_output_ptr = scanline_buffers[VBLANK] + picture_hshift_bytes / 4;
    const uint32_t color = ((palette[color888] << 16) | palette[color888]) & 0x3f3f3f3f | 0xc0c0c0c0;
    for (int i = 160; i--;) {
        *scanline_output_ptr++ = color;
    }
}


// -----------------------------------------------------------------------------
void graphics_set_palette(const uint8_t index, const uint32_t color) {
    // color is RGB888, convert to 2-bit-per-component (0..3) and pack into 6-bit value
    const uint8_t r = ((color >> 16) & 0xff) >> 6;
    const uint8_t g = ((color >> 8) & 0xff) >> 6;
    const uint8_t b = (color & 0xff) >> 6;

    const uint8_t rgb = (r << 4) | (g << 2) | b;

    palette[index] = (rgb << 8 | rgb) & 0x3f3f | 0xc0c0;
}

// -----------------------------------------------------------------------------
void graphics_init() {
    // --- initialize PIO ---
    // On RP2350B
    pio_set_gpio_base(PIO_VGA, 16);
    const uint offset = pio_add_program(PIO_VGA, &pio_program_VGA);
    const uint sm = pio_claim_unused_sm(PIO_VGA, true);

    for (int i = 0; i < VGA_PINS; i++) {
        gpio_init(VGA_BASE_PIN + i);
        gpio_set_dir(VGA_BASE_PIN + i, GPIO_OUT);
        pio_gpio_init(PIO_VGA, VGA_BASE_PIN + i);
    }

    pio_sm_set_consecutive_pindirs(PIO_VGA, sm, VGA_BASE_PIN, VGA_PINS, true);

    pio_sm_config cfg = pio_get_default_sm_config();
    sm_config_set_out_pin_base(&cfg, VGA_BASE_PIN);
    sm_config_set_wrap(&cfg, offset + 0, offset + (pio_program_VGA.length - 1));
    sm_config_set_fifo_join(&cfg, PIO_FIFO_JOIN_TX);
    sm_config_set_out_shift(&cfg, true, true, 32);
    sm_config_set_out_pins(&cfg, VGA_BASE_PIN, VGA_PINS);

    pio_sm_init(PIO_VGA, sm, offset, &cfg);
    pio_sm_set_enabled(PIO_VGA, sm, true);
    pio_sm_set_clkdiv(PIO_VGA, sm, clock_get_hz(clk_sys) / pixel_clock); // set PIO clock divider approximately for 25.175MHz pixel clock

    // --- initialize DMA channels ---
    dma_ctrl_channel = dma_claim_unused_channel(true);
    dma_data_channel = dma_claim_unused_channel(true);

    // main data channel config
    dma_channel_config data_cfg = dma_channel_get_default_config(dma_data_channel);
    channel_config_set_transfer_data_size(&data_cfg, DMA_SIZE_32);
    channel_config_set_read_increment(&data_cfg, true);
    channel_config_set_write_increment(&data_cfg, false);

    channel_config_set_dreq(&data_cfg, (PIO_VGA == pio0 ? DREQ_PIO0_TX0 : DREQ_PIO1_TX0) + sm);
    channel_config_set_chain_to(&data_cfg, dma_ctrl_channel);

    dma_channel_configure(
        dma_data_channel,
        &data_cfg,
        &PIO_VGA->txf[sm], // write address (PIO TX FIFO)
        scanline_buffers[VBLANK], // read address (will be updated)
        scanline_bytes / 4,
        false);

    // control channel config
    dma_channel_config ctrl_cfg = dma_channel_get_default_config(dma_ctrl_channel);
    channel_config_set_transfer_data_size(&ctrl_cfg, DMA_SIZE_32);
    channel_config_set_read_increment(&ctrl_cfg, false);
    channel_config_set_write_increment(&ctrl_cfg, false);
    channel_config_set_chain_to(&ctrl_cfg, dma_data_channel);

    dma_channel_configure(
        dma_ctrl_channel,
        &ctrl_cfg,
        &dma_hw->ch[dma_data_channel].read_addr, // write address (point to read_addr of a data channel)
        &scanline_buffers[VBLANK], // read address (pattern pointer)
        1,
        false);

    // default graphics mode
    graphics_set_mode(CGA_320x200x4);

    // IRQ setup
    irq_set_exclusive_handler(VGA_DMA_IRQ, vga_scanline_dma);
    dma_channel_set_irq0_enabled(dma_ctrl_channel, true);
    irq_set_enabled(VGA_DMA_IRQ, true);

    dma_start_channel_mask(1u << dma_data_channel);

    // --- prepare text palette expanded table ---
    for (int i = 0; i < 256; i++) {
        const uint8_t c1 = textmode_palette[i & 0xF];
        const uint8_t c0 = textmode_palette[i >> 4];

        textmode_palette_lut[i * 4 + 0] = c0 | (c0 << 8);
        textmode_palette_lut[i * 4 + 1] = c1 | (c0 << 8);
        textmode_palette_lut[i * 4 + 2] = c0 | (c1 << 8);
        textmode_palette_lut[i * 4 + 3] = c1 | (c1 << 8);
    }

    // assign buffer pointers into a single large array
    for (int i = 0; i < SCANLINE_BUFFERS; i++) {
        scanline_buffers[i] = &scanline_buffer_mem[i * (scanline_bytes / 4)];
    }

    // prepare templates
    constexpr uint8_t tmpl_active_video  = 0b11000000; // TMPL_LINE8
#ifdef VGA_CSYNC
    // --- CSYNC MODE (VHBBGGRR) ---
    // HSync (bit 6) несет композитный сигнал.
    // VSync (bit 7) всегда 1 (отключен/неактивен).
    // Логика: XNOR (стандартная композитная синхра для VGA входов типа GBS-C/Scart).
    constexpr uint8_t tmpl_hsync         = 0b10000000; // Обычная строка: импульс HSync = 0 (Bit6=0, Bit7=1)
    constexpr uint8_t tmpl_vsync         = 0b10000000; // VSync строка: фон = 0 (Bit6=0, Bit7=1)
    constexpr uint8_t tmpl_video_hv_sync = 0b11000000; // VSync строка: импульс (serration) = 1 (Bit6=1, Bit7=1)
#else
    // --- STANDARD VGA MODE (VHBBGGRR) ---
    const uint8_t tmpl_hsync = tmpl_active_video ^ 0b01000000; // 10... (V=1, H=0)
    const uint8_t tmpl_vsync = tmpl_active_video ^ 0b10000000; // 01... (V=0, H=1)
    const uint8_t tmpl_video_hv_sync = tmpl_active_video ^ 0b11000000; // 00... (V=0, H=0)
#endif

    // base pointer to buffer memory as bytes
    auto base_ptr = scanline_buffers[VBLANK];

    // пустая строка (active video background)
    memset(base_ptr, tmpl_active_video, scanline_bytes);

    // выровненная синхра вначале
    memset(base_ptr, tmpl_hsync, hsync_pulse_width_bytes);

    // кадровая синхра (vsync)
    base_ptr = scanline_buffers[VSYNC];
    memset(base_ptr, tmpl_vsync, scanline_bytes);
    memset(base_ptr, tmpl_video_hv_sync, hsync_pulse_width_bytes);

    // заготовки для строк с изображением (copy blank template)
    base_ptr = scanline_buffers[IMAGE];
    memcpy(base_ptr, scanline_buffers[VBLANK], scanline_bytes);
    /*
    base_ptr = scanline_buffers[EVEN];
    memcpy(base_ptr, scanline_buffers[BLANK], scanline_bytes);
    */
}

// -----------------------------------------------------------------------------
void graphics_set_mode(const enum graphics_mode_t mode) {
    graphics_mode = mode;
}
