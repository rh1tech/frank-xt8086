#include <math.h>

#include <hardware/dma.h>
#include <hardware/irq.h>
#include <hardware/pio.h>
#include "graphics.h"
#include "state.h"
#include "vgacard.h"

/*
 * Where the text modes fetch characters from.
 *
 * VIDEORAM normally — the page the 8086 writes. The on-screen display
 * points it at its own buffer so a menu can be shown over a guest that is
 * still running, rather than drawn into the page the guest owns and then
 * scribbled over by the next thing it prints.
 *
 * Read once per scanline, and only ever changed by core 0 between frames,
 * so no synchronisation is needed beyond it being a single aligned word.
 */
uint8_t *vga_text_source = NULL;

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

/*
 * Display timing, chosen per mode rather than fixed.
 *
 * Everything here used to be constexpr, which was fine while every mode
 * was 640 pixels across. Hercules is 720, and no arrangement of a
 * 640-pixel line will show it: the pixels have to be narrower and the
 * line longer.
 *
 * Both entries run the line at the same 31.469 kHz, which is what makes
 * switching between them safe -- a display relocks on the pixel count,
 * not on a new horizontal frequency. 720x400 at 70 Hz would have been the
 * obvious choice for Hercules, being the mode every VGA card uses for DOS
 * text, but it wants a positive vertical sync where this hardware emits a
 * negative one, and the converter on this board would not lock to it.
 * 720x480 at 60 Hz keeps the vertical timing we already know works and
 * only widens the line. That is measured, not assumed: at 720x400 the
 * capture end reported no signal at all, and at 720x480 it locked.
 */
typedef struct {
    int total_scanlines;
    int visible_scanlines;
    int vsync_start_line;
    int vsync_end_line;
    int pixel_clock;
    int hsync_offset_bytes;      // HS_SHIFT
    int hsync_pulse_width_bytes; // HS_SIZE, the back porch
    int scanline_bytes;          // whole line, sync and porches included
    int visible_bytes;           // of which picture
} vga_timing_t;

static const vga_timing_t timing_640x480 = {
    525, 480, 490, 491, 25'175'000, 328 * 2, 48 * 2, 400 * 2, 640,
};

static const vga_timing_t timing_720x480 = {
    525, 480, 490, 491, 28'322'000, 738, 108, 900, 720,
};

// Buffers are sized for the longest line either timing needs.
#define MAX_SCANLINE_BYTES 900

static vga_timing_t vt = timing_640x480;

// Where the picture starts within a line buffer, in 32-bit words. Held
// separately because the scanline interrupt reads it every line.
static int picture_hshift_words;

#if defined(VGA_CSYNC)
constexpr int VGA_PINS = 7;
#define PICTURE_HSHIFT_BYTES (vt.scanline_bytes - vt.hsync_offset_bytes + 12)
#else
constexpr int VGA_PINS = 8;
#define PICTURE_HSHIFT_BYTES (vt.scanline_bytes - vt.hsync_offset_bytes)
#endif

// scanline_buffers: [0]=blank, [1]=vsync, [2]=image
enum { VBLANK, VSYNC, IMAGE };
#define SCANLINE_BUFFERS 3
static uint32_t *scanline_buffers[SCANLINE_BUFFERS] __attribute__((aligned(4))) = {0};
static uint32_t scanline_buffer_mem[MAX_SCANLINE_BYTES * SCANLINE_BUFFERS] __attribute__((aligned(32)));

// --- DMA / PIO channels ---
static uint sm;              // the VGA state machine, needed to retime it
static int dma_ctrl_channel;
static int dma_data_channel;

static uint16_t palette[256] __attribute__((aligned(4)));

/*
 * CGA composite artifact colour.
 *
 * One entry per four-pixel group, already replicated four times so a
 * whole colour cell is a single 32-bit store.
 *
 * The colours are derived rather than tabulated. A CGA's 14.318 MHz pixel
 * clock is exactly four times the 3.5795 MHz NTSC colour subcarrier, so
 * four consecutive pixels span one full cycle and amount to sampling it
 * at 0, 90, 180 and 270 degrees. Demodulating those four samples the way
 * a television does gives the colour that television would have shown:
 *
 *   Y = (s0 + s1 + s2 + s3) / 4      luma, just the average
 *   I = (s0 - s2) / 2                the 0/180 axis
 *   Q = (s1 - s3) / 2                the 90/270 axis
 *
 * then the standard YIQ to RGB matrix. That is why 1010 and 0101 come out
 * as opposite hues at the same brightness while 1100 and 0011 do not:
 * they differ in phase, not in how many pixels are lit.
 *
 * COMPOSITE_HUE_DEG is the burst phase. On real hardware it depends on
 * the card and the monitor's tint control, so there is no single correct
 * value; this is the knob to turn if the colours come out rotated against
 * software you know.
 */
#define COMPOSITE_HUE_DEG   0.0f
#define COMPOSITE_SATURATION 1.5f

static uint32_t composite_quad[16] __attribute__((aligned(4)));

/*
 * EGA planar, from murm386.
 *
 * The card's memory keeps the four planes interleaved a byte at a time,
 * so one 32-bit fetch carries all four bits of eight pixels -- but
 * scrambled: plane n's bit for pixel p sits in byte n, bit 7-p. Turning
 * that into eight nibbles is what the lookup table is for. spread8 takes
 * a byte and spaces its bits out four apart; OR the four planes together
 * with one, two and three bits of shift and eight pixels fall out in
 * order, most significant first.
 *
 * Doing it a bit at a time would be thirty-two shifts a word. This is
 * four loads and three ORs.
 */
static uint32_t spread8_lut[256];

static void build_spread8_lut(void) {
    for (int i = 0; i < 256; i++) {
        uint32_t v = 0;
        for (int b = 0; b < 8; b++)
            if (i & (1 << b)) v |= 1u << (b * 4);
        spread8_lut[i] = v;
    }
}

__force_inline static uint32_t ega_pack8(const uint32_t planes) {
    return  spread8_lut[ planes        & 0xFFu]
         | (spread8_lut[(planes >>  8) & 0xFFu] << 1)
         | (spread8_lut[(planes >> 16) & 0xFFu] << 2)
         | (spread8_lut[ planes >> 24        ] << 3);
}

// Filled once a frame from the card; see vgacard_get_frame().
vgacard_frame_t vga_frame;

static void build_composite_palette(void) {
    const float hue = COMPOSITE_HUE_DEG * (float)M_PI / 180.0f;
    const float ch = cosf(hue), sh = sinf(hue);

    for (int idx = 0; idx < 16; idx++) {
        // Bit 3 is the leftmost pixel, and therefore phase 0.
        const float s0 = (idx >> 3) & 1, s1 = (idx >> 2) & 1;
        const float s2 = (idx >> 1) & 1, s3 = (idx >> 0) & 1;

        const float Y  = (s0 + s1 + s2 + s3) * 0.25f;
        const float i0 = (s0 - s2) * 0.5f;
        const float q0 = (s1 - s3) * 0.5f;

        const float I = (i0 * ch - q0 * sh) * COMPOSITE_SATURATION;
        const float Q = (i0 * sh + q0 * ch) * COMPOSITE_SATURATION;

        float rgb[3] = {
            Y + 0.956f * I + 0.621f * Q,
            Y - 0.272f * I - 0.647f * Q,
            Y - 1.106f * I + 1.703f * Q,
        };

        uint8_t q[3];
        for (int c = 0; c < 3; c++) {
            float v = rgb[c] < 0.0f ? 0.0f : (rgb[c] > 1.0f ? 1.0f : rgb[c]);
            // Six-bit ladder: two bits a channel, so 0..3.
            q[c] = (uint8_t)(v * 3.0f + 0.5f);
        }

        const uint8_t byte = (uint8_t)(((q[0] << 4) | (q[1] << 2) | q[2]) & 0x3F) | 0xC0;
        composite_quad[idx] = (uint32_t)byte * 0x01010101u;
    }
}
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

/*
 * Is the cursor on this glyph row?
 *
 * R10 is not just a scanline. Bits 6-5 are the cursor mode -- 00 steady,
 * 01 not displayed, 10 blink at 1/16 the field rate, 11 at 1/32 -- and
 * only bits 4-0 are the start line. R11 is five bits too.
 *
 * Comparing the whole byte as a scanline broke hiding and sizing at once.
 * The BIOS hides the cursor with INT 10h AH=01 and CH bit 5 set, which
 * puts 0x20 in R10. Read as a number that is 32, larger than any end
 * line, so the start > end wrap-around case fired and drew a cursor from
 * row 0 to cursor_end -- a fat block, exactly where there was meant to be
 * nothing. Any program setting a mode bit got a cursor of a size it never
 * asked for, which is why it kept changing.
 */
__force_inline static bool cursor_on_line(const uint8_t glyph_line,
                                          const uint8_t screen_y) {
    if (likely(screen_y != mc6845.cursor_y)) return false;

    // Mode 01 is "cursor non-display". Nothing else about R10 matters.
    if (((mc6845.r.cursor_start >> 5) & 3u) == 1u) return false;

    const uint8_t start = mc6845.r.cursor_start & 0x1Fu;
    const uint8_t end   = mc6845.r.cursor_end   & 0x1Fu;

    // start > end wraps on a real 6845 rather than being invalid.
    return likely(start <= end) ? (glyph_line >= start && glyph_line <= end)
                                : (glyph_line >= start || glyph_line <= end);
}

void __time_critical_func() vga_scanline_dma() {
    static uint32_t scanline = 0; // previously screen_line

    // acknowledge interrupt for control channel
    dma_hw->ints0 = 1u << dma_ctrl_channel;

    // advance scanline/frame counters
    scanline++;
    if (unlikely(scanline == vt.total_scanlines)) {
        scanline = 0;
    }

    // If outside visible area - mark output as finished
    if (unlikely(scanline >= vt.visible_scanlines)) {
        const int is_vsync = (scanline >= vt.vsync_start_line && scanline <= vt.vsync_end_line) ? VSYNC : VBLANK;
        dma_channel_set_read_addr(dma_ctrl_channel, &scanline_buffers[is_vsync], false);
        port3DA = 9;
        return;
    }

    // choose odd/even image buffer pointer
    uint16_t y = scanline;

    /*
     * Hercules draws a line per scanline; everything else doubles.
     *
     * The CGA modes are 200 lines shown on 480, so each is drawn once and
     * held for two. Hercules is 348, or whatever else the CRTC has been
     * programmed for -- Planet X3 asks for 300 -- and doubling caps that
     * at the 240 lines half of 480 allows. The top of the picture would
     * be stretched over the whole screen and the rest simply missing.
     */
    const bool tall = graphics_mode == HERC_720x348x2 ||
                      graphics_mode == HERC_720x348x2_90;
    /*
     * Hercules rows are four scanlines, whatever R9 says.
     *
     * The bank counter on the card is two bits wide and the framebuffer
     * is four interleaved banks, so a row is four lines and the picture
     * is v_displayed * 4. Taking the height from R9 instead is what cut
     * Planet X3 off mid-menu: it programs R9 = 2, which would make the
     * rows three lines and the picture 300 rather than 400, and the
     * bottom quarter simply was not drawn. The addressing here has always
     * used four; only the line count disagreed.
     *
     * A stock 720x348 sets R9 = 3 and 87 rows, so four is right there
     * too -- this is not a special case, it is the constant the hardware
     * actually uses.
     */
    const uint32_t drawn_lines = tall
            ? (uint32_t)mc6845_mda.r.v_displayed * 4u
            : (uint32_t)mc6845.r.v_displayed * (mc6845.r.max_scanline_addr + 1) * 2u;

    // If line index beyond prepared image area — fall back to blank
    if (unlikely(scanline >= drawn_lines)) {
        dma_channel_set_read_addr(dma_ctrl_channel, &scanline_buffers[VBLANK], false);
        port3DA = 1;
        return;
    }
    // Non-interlace: skip odd sublines and fold y
    if (!tall && likely((mc6845.r.interlace_mode & 1) == 0)) {
        if (y & 1) {
            port3DA = 1;
            return;
        }
        // is_even = 2;
        y >>= 1; // 200 logical lines
    }
    uint32_t **scanline_output_ptr = &scanline_buffers[IMAGE];
    uint32_t *__restrict scanline_output_32 = *scanline_output_ptr + picture_hshift_words;


    // activate output for visible lines
    port3DA = 0;
    switch (graphics_mode) {
        case TEXTMODE_40x25_COLOR:
        case TEXTMODE_40x25_BW: {
            // "слой" символа
            uint8_t char_scanlines = mc6845.r.max_scanline_addr;
            const uint8_t glyph_line = y & char_scanlines;
            char_scanlines++;

            /*
             * Sixteen-line characters mean the 8x16 font.
             *
             * A CGA draws eight-line characters and a VGA in text mode
             * draws sixteen, and the glyph fetch multiplies by the
             * height: at sixteen it was indexing an eight-line font well
             * past its end, so every character on screen came out of
             * whatever followed it in memory.
             */
            const uint8_t *__restrict fnt =
                    (char_scanlines >= 16u) ? font_8x16 : font_8x8;
            const uint8_t screen_y = y / char_scanlines;

            //указатель откуда начать считывать символы
            const uint8_t *__restrict src = vga_text_source ? vga_text_source : VIDEORAM;
            const uint32_t *__restrict text_buffer_line = (uint32_t *) &src[mc6845.vram_offset + __fast_mul(screen_y, mc6845.r.h_displayed << 1)];
            __builtin_prefetch(text_buffer_line);

            const bool is_cursor_line_active = cursor_on_line(glyph_line, screen_y);

            // Предвычисление позиции курсора
            const int cursor_char_x = is_cursor_line_active ? mc6845.cursor_x : -1;

            for (int char_x = 0; char_x < mc6845.r.h_displayed; char_x += 2) {
                uint32_t dword = *text_buffer_line++;

                // Первый символ из пачки
                uint8_t glyph_pixels = fnt[(dword & 0xFF) * char_scanlines + glyph_line];
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
                glyph_pixels = fnt[(dword & 0xFF) * char_scanlines + glyph_line];
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

            /*
             * Sixteen-line characters mean the 8x16 font.
             *
             * A CGA draws eight-line characters and a VGA in text mode
             * draws sixteen, and the glyph fetch multiplies by the
             * height: at sixteen it was indexing an eight-line font well
             * past its end, so every character on screen came out of
             * whatever followed it in memory.
             */
            const uint8_t *__restrict fnt =
                    (char_scanlines >= 16u) ? font_8x16 : font_8x8;
            const uint8_t screen_y = y / char_scanlines;

            //указатель откуда начать считывать символы
            const uint8_t *__restrict src = vga_text_source ? vga_text_source : VIDEORAM;
            const uint32_t *__restrict text_buffer_line = (uint32_t *) &src[
                (mc6845.vram_offset + __fast_mul(screen_y, mc6845.r.h_displayed << 1)) & 0x3FFF];
            __builtin_prefetch(text_buffer_line);
            const bool is_cursor_line_active =
                    unlikely(mc6845.cursor_blink_state) &&
                    cursor_on_line(glyph_line, screen_y);

            // Предвычисление позиции курсора
            const int cursor_char_x = is_cursor_line_active ? mc6845.cursor_x : -1;

            for (int char_x = 0; char_x < mc6845.r.h_displayed; char_x += 2) {
                uint32_t dword = *text_buffer_line++;

                // Первый символ из пачки
                uint8_t glyph_pixels = fnt[(dword & 0xFF) * char_scanlines + glyph_line];
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
                glyph_pixels = fnt[(dword & 0xFF) * char_scanlines + glyph_line];
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
            /*
             * vram_offset, like every other mode. Without it this showed
             * page 0 for ever: a game that draws into a second page and
             * points the CRTC start address at it got a screen that had
             * never been drawn. Planet X3 does that once it leaves its
             * title screen, which is why the title appeared and the game
             * did not.
             *
             * The mask needs the parentheses too. `a + b & M` masks only
             * b, so the wrap this looked like it was doing was never
             * happening -- harmless while the offset was always zero, not
             * harmless now.
             */
            const uint32_t *__restrict cga_row = (uint32_t *) &VIDEORAM[
                    (mc6845.vram_offset + __fast_mul(y >> 1, 80) + ((y & 1) << 13)) & 0x3FFF];
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
        /*
         * Same bytes as CGA_640x200x2 above, read four bits at a time
         * instead of one. Eighty bytes a line is 160 colour cells, each
         * four pixels wide, which fills the same 640.
         */
        case COMPOSITE_160x200x16: {
            const uint8_t *__restrict cga_row =
                    &VIDEORAM[(mc6845.vram_offset + __fast_mul(y >> 1, 80) +
                               ((y & 1) << 13)) & 0x3FFF];
            __builtin_prefetch(cga_row);

            for (int x = 80; x--;) {
                const uint8_t b = *cga_row++;
                *scanline_output_32++ = composite_quad[b >> 4];
                *scanline_output_32++ = composite_quad[b & 15];
            }
            break;
        }
        case TGA_160x200x16: {
            const uint32_t *__restrict tga_row = (uint32_t *) &VIDEORAM[
                    (cga.tandy_crt_base + mc6845.vram_offset +
                     __fast_mul(y >> 1, 80) + ((y & 1) << 13)) & (VIDEORAM_SIZE - 1)];
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
            const uint32_t *__restrict tga_row = (uint32_t *) &VIDEORAM[
                    (cga.tandy_crt_base + mc6845.vram_offset +
                     __fast_mul(y >> 2, 160) + ((y & 3) << 13)) & (VIDEORAM_SIZE - 1)];
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

        /*
         * Tandy 640x200x16.
         *
         * The same shape as the 320-wide mode above and for the same
         * reason: four-way scanline interleave, one nibble a pixel. Only
         * the numbers change -- 320 bytes a line rather than 160, and a
         * 16K bank stride rather than 8K. Four banks of 16K is the whole
         * 64K, and 4 * 50 * 320 = 64,000 of it is picture.
         *
         * 640 pixels means one output byte per pixel, where the 320-wide
         * modes write a palette entry twice.
         */
        /*
         * Hercules, 720x348 mono.
         *
         * Four-way scanline interleave, like the Tandy modes, but in 8K
         * banks: line L lives at (L & 3) * 8192 + (L >> 2) * 90. Ninety
         * bytes is 720 bits, one per pixel, and 4 * 87 * 90 = 31,320
         * bytes is the whole picture.
         *
         * 348 lines are drawn into 480, so the vertical position is up to
         * whatever the CRTC asks for; nothing is scaled.
         *
         * Bit 7 of each byte is the leftmost pixel, so the word is
         * reversed once and then consumed from the bottom, which is what
         * CGA_640x200x2 does with the same problem.
         */
        /*
         * Hercules, one bit a pixel, four-way interleaved in 8K banks:
         * line L at (L & 3) * 8192 + (L >> 2) * stride.
         *
         * The stride is the CRTC's displayed-character count, not a
         * constant. A stock Hercules is 90 characters, 720 pixels, but
         * the card is just a 6845 driving a bitmap and software
         * reprograms it freely -- Planet X3 runs 640x300, which is 80.
         * Hard-coding 90 tore its picture into diagonal stripes, because
         * every line was read ten bytes further along than the last.
         */
        case HERC_720x348x2:
        case HERC_720x348x2_90: {
            // Two bytes a character. In graphics the 6845 counts
            // character clocks of sixteen pixels, not eight: a stock
            // Hercules programs 45 for its 720, and Planet X3 programs 40
            // for 640. Reading it as one byte drew exactly half the
            // picture across half the screen.
            const uint32_t stride = (uint32_t)mc6845_mda.r.h_displayed * 2u;
            const uint8_t *__restrict herc_row = &VIDEORAM[
                    HERC_VRAM_BASE +
                    ((mc6845_mda.vram_offset + ((y & 3) << 13) +
                      __fast_mul(y >> 2, stride)) & 0x7FFFu)];
            __builtin_prefetch(herc_row);

            // Bit 7 is the leftmost pixel. Masked to a byte each: the
            // palette holds the colour in both halves of a 16-bit word,
            // and letting those overlap would blend neighbouring pixels
            // together instead of placing them side by side.
            const uint8_t c0 = (uint8_t)palette[0];
            const uint8_t c1 = (uint8_t)palette[1];

            for (uint32_t x = 0; x < stride; x++) {
                const uint8_t b = *herc_row++;
                *scanline_output_32++ =
                        (uint32_t)((b & 0x80) ? c1 : c0)        |
                        (uint32_t)((b & 0x40) ? c1 : c0) <<  8  |
                        (uint32_t)((b & 0x20) ? c1 : c0) << 16  |
                        (uint32_t)((b & 0x10) ? c1 : c0) << 24;
                *scanline_output_32++ =
                        (uint32_t)((b & 0x08) ? c1 : c0)        |
                        (uint32_t)((b & 0x04) ? c1 : c0) <<  8  |
                        (uint32_t)((b & 0x02) ? c1 : c0) << 16  |
                        (uint32_t)((b & 0x01) ? c1 : c0) << 24;
            }
            break;
        }
        /*
         * Mode 13h, from the card. Chain-4 puts four consecutive pixels
         * in the four planes of one word, so a single fetch is four
         * pixels in order and no unpacking is needed at all.
         */
        case VGA_320x200x256: {
            const uint32_t stride = vga_frame.line_offset > 0
                    ? (uint32_t)vga_frame.line_offset * 2u : 80u;
            uint32_t offset = vga_frame.start_addr + (uint32_t)y * stride;
            offset &= 0xFFFFu;

            const uint32_t *__restrict src = vgacard_planes() + offset;
            for (int i = 0; i < 80; i++) {
                const uint32_t px = src[i];
                *scanline_output_32++ = palette[ px        & 0xFF] |
                                        (uint32_t)palette[(px >>  8) & 0xFF] << 16;
                *scanline_output_32++ = palette[(px >> 16) & 0xFF] |
                                        (uint32_t)palette[ px >> 24        ] << 16;
            }
            break;
        }
        /*
         * EGA, 16 colours out of four planes.
         *
         * Geometry comes from the card, not from the mode number: EGA
         * modes are 320x200, 640x200 or 640x350, and software moves the
         * line offset and start address as it pleases -- scrolling is
         * done by moving the start address, so a fixed one would give a
         * picture that never moves.
         */
        case EGA_320x200x16x4:
        case EGA_640x200x16x4:
        case EGA_640x350x16x4: {
            const int height = vga_frame.height > 0 ? vga_frame.height : 200;

            // 200-line modes are doubled into 400; 350 is scaled to fit.
            const uint32_t src_line = (height <= 200)
                    ? (uint32_t)y
                    : ((uint32_t)y * 2u * (uint32_t)height) / 400u;

            /*
             * Past the bottom of the picture, paint the line rather than
             * leaving it: the scanline buffer still holds the last thing
             * written there, and its bytes carry the sync bits.
             */
            if (src_line >= (uint32_t)height) {
                const uint8_t bg = vga_frame.palette[0];
                const uint32_t blank = (uint32_t)bg * 0x01010101u;
                for (int i = 0; i < 160; i++) scanline_output_32[i] = blank;
                break;
            }

            const uint32_t stride = vga_frame.line_offset > 0
                    ? (uint32_t)vga_frame.line_offset * 2u
                    : (uint32_t)vga_frame.width / 8u;

            uint32_t offset;
            if (vga_frame.line_compare >= 0 && src_line >= (uint32_t)vga_frame.line_compare)
                offset = (src_line - (uint32_t)vga_frame.line_compare) * stride;
            else
                offset = (uint32_t)vga_frame.start_addr + src_line * stride;
            offset &= 0xFFFFu;

            const uint32_t *__restrict src = vgacard_planes() + offset;
            const uint8_t  *__restrict pal = vga_frame.palette;
            const bool doubled = vga_frame.width <= 320;

            int words = vga_frame.width / 8;
            if (words > 80) words = 80;

            /*
             * Horizontal pixel panning, from the attribute controller's
             * register 0x13. Three bits, so a line can start up to seven
             * pixels into the first byte.
             *
             * This is the other half of hardware scrolling. The CRTC's
             * start address moves the picture a byte -- eight pixels --
             * at a time, and everything between is this register. Ignoring
             * it, as this did, means a game scrolling smoothly jumps a
             * whole byte at a time and sits at the wrong offset for seven
             * frames out of eight.
             *
             * Taken from murm386's renderer: shift the eight pixels up by
             * four bits per pixel of pan and pull the top of the next
             * byte in underneath.
             */
            const int panning = vga_frame.panning & 7;
            const int shift   = panning * 4;

            for (int i = 0; i < words; i++) {
                uint32_t px = ega_pack8(src[i]);
                if (panning) {
                    const uint32_t next = ega_pack8(src[i + 1]);
                    px = (px << shift) | (next >> (32 - shift));
                }
                const uint32_t c0 = pal[ px >> 28        ], c1 = pal[(px >> 24) & 0xF];
                const uint32_t c2 = pal[(px >> 20) & 0xF], c3 = pal[(px >> 16) & 0xF];
                const uint32_t c4 = pal[(px >> 12) & 0xF], c5 = pal[(px >>  8) & 0xF];
                const uint32_t c6 = pal[(px >>  4) & 0xF], c7 = pal[ px        & 0xF];

                if (doubled) {
                    *scanline_output_32++ = c0 | c0 << 8 | c1 << 16 | c1 << 24;
                    *scanline_output_32++ = c2 | c2 << 8 | c3 << 16 | c3 << 24;
                    *scanline_output_32++ = c4 | c4 << 8 | c5 << 16 | c5 << 24;
                    *scanline_output_32++ = c6 | c6 << 8 | c7 << 16 | c7 << 24;
                } else {
                    *scanline_output_32++ = c0 | c1 << 8 | c2 << 16 | c3 << 24;
                    *scanline_output_32++ = c4 | c5 << 8 | c6 << 16 | c7 << 24;
                }
            }
            break;
        }

        case TGA_640x200x16: {
            const uint8_t *__restrict tga_row = &VIDEORAM[
                    (cga.tandy_crt_base + mc6845.vram_offset +
                     __fast_mul(y >> 2, 320) + ((y & 3) << 14)) & (VIDEORAM_SIZE - 1)];
            __builtin_prefetch(tga_row);

            uint8_t *__restrict out = (uint8_t *) scanline_output_32;
            for (int x = 320; x--;) {
                const uint8_t two_pixels = *tga_row++;
                *out++ = (uint8_t)palette[two_pixels >> 4];
                *out++ = (uint8_t)palette[two_pixels & 15];
            }
            break;
        }
    }

    dma_channel_set_read_addr(dma_ctrl_channel, scanline_output_ptr, false);
    port3DA = 1; // no more data shown
}

void graphics_set_bgcolor(const uint32_t color888) {
    uint32_t *scanline_output_ptr = scanline_buffers[VBLANK] + picture_hshift_words;
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
/*
 * Paint the sync and blanking patterns for the current timing.
 *
 * Called again whenever the timing changes, because every one of these
 * lengths comes from it.
 */
static void build_sync_templates(void) {
    picture_hshift_words = PICTURE_HSHIFT_BYTES / 4;

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
    memset(base_ptr, tmpl_active_video, vt.scanline_bytes);

    // выровненная синхра вначале
    memset(base_ptr, tmpl_hsync, vt.hsync_pulse_width_bytes);

    // кадровая синхра (vsync)
    base_ptr = scanline_buffers[VSYNC];
    memset(base_ptr, tmpl_vsync, vt.scanline_bytes);
    memset(base_ptr, tmpl_video_hv_sync, vt.hsync_pulse_width_bytes);

    // заготовки для строк с изображением (copy blank template)
    base_ptr = scanline_buffers[IMAGE];
    memcpy(base_ptr, scanline_buffers[VBLANK], vt.scanline_bytes);
}

/*
 * (Re)configure the two chained DMA channels.
 *
 * The control channel rewrites the data channel's read address and the
 * data channel chains back to it, so the pair free-runs a line at a time.
 * Both descriptions depend on the line length, which is why this is a
 * function and not just part of init: changing timing has to rewrite
 * them.
 */
static void configure_vga_dma(void) {
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
        vt.scanline_bytes / 4,
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

}

void graphics_init() {
    build_composite_palette();
    build_spread8_lut();

    // --- initialize PIO ---
    // On RP2350B
    pio_set_gpio_base(PIO_VGA, 16);
    const uint offset = pio_add_program(PIO_VGA, &pio_program_VGA);
    sm = pio_claim_unused_sm(PIO_VGA, true);

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
    pio_sm_set_clkdiv(PIO_VGA, sm, (float)clock_get_hz(clk_sys) / vt.pixel_clock);

    // --- initialize DMA channels ---
    dma_ctrl_channel = dma_claim_unused_channel(true);
    dma_data_channel = dma_claim_unused_channel(true);

    configure_vga_dma();

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
        scanline_buffers[i] = &scanline_buffer_mem[i * (MAX_SCANLINE_BYTES / 4)];
    }

    build_sync_templates();
}

// -----------------------------------------------------------------------------
/*
 * Switch display timing.
 *
 * Everything stops first. The scanline interrupt, the two chained DMA
 * channels and the state machine are all mid-flight in a line whose
 * length is about to change, and letting any of them carry a stale count
 * across the change is how the picture is lost for good rather than for a
 * frame -- the chain simply stops and nothing restarts it.
 *
 * A few frames of black while this happens is the correct cost, and it
 * only happens when a guest changes to or from a 720-pixel mode.
 */
static void apply_timing(const vga_timing_t *nt) {
    if (nt->scanline_bytes == vt.scanline_bytes &&
        nt->pixel_clock    == vt.pixel_clock) return;

    irq_set_enabled(VGA_DMA_IRQ, false);

    /*
     * Clear both enables before aborting.
     *
     * These two channels chain to each other, and aborting one while the
     * other can still trigger it means the abort never settles: the pair
     * restarts itself from the chain and the reconfiguration lands on a
     * channel that is already running again. Taking the enables away
     * first stops the chain dead, and configure_vga_dma() writes them
     * back as part of rewriting both descriptions.
     */
    hw_clear_bits(&dma_hw->ch[dma_data_channel].al1_ctrl, DMA_CH0_CTRL_TRIG_EN_BITS);
    hw_clear_bits(&dma_hw->ch[dma_ctrl_channel].al1_ctrl, DMA_CH0_CTRL_TRIG_EN_BITS);
    dma_channel_abort(dma_data_channel);
    dma_channel_abort(dma_ctrl_channel);
    dma_hw->ints0 = (1u << dma_ctrl_channel) | (1u << dma_data_channel);

    pio_sm_set_enabled(PIO_VGA, sm, false);
    pio_sm_clear_fifos(PIO_VGA, sm);

    vt = *nt;

    pio_sm_set_clkdiv(PIO_VGA, sm, (float)clock_get_hz(clk_sys) / vt.pixel_clock);
    build_sync_templates();
    configure_vga_dma();

    pio_sm_set_enabled(PIO_VGA, sm, true);
    dma_channel_set_irq0_enabled(dma_ctrl_channel, true);
    irq_set_enabled(VGA_DMA_IRQ, true);

    // Started the same way init does: the data channel first, which
    // chains to the control channel and from there runs on its own.
    dma_start_channel_mask(1u << dma_data_channel);
}

/*
 * The palette a VGA comes up with in mode 13h.
 *
 * Software usually replaces it -- fading one in is half of what the DAC
 * gets used for -- but something that assumes the default and draws
 * before setting one would otherwise get a screen of black. The standard
 * layout is sixteen CGA colours, sixteen greys, then a large colour
 * block; this approximates the third part with an RGB cube, which on a
 * ladder with two bits a channel is as much as can be told apart anyway.
 */
static void build_vga_default_palette(void) {
    for (int i = 0; i < 16; i++) graphics_set_palette(i, cga_palette[i]);

    for (int i = 0; i < 16; i++) {
        const uint32_t v = (uint32_t)(i * 17);
        graphics_set_palette(16 + i, v << 16 | v << 8 | v);
    }

    // 6 * 6 * 6 = 216 entries from 32, and the last eight left black as
    // the real default leaves them.
    int idx = 32;
    for (int r = 0; r < 6; r++)
        for (int g = 0; g < 6; g++)
            for (int b = 0; b < 6; b++)
                graphics_set_palette(idx++, (uint32_t)(r * 51) << 16 |
                                            (uint32_t)(g * 51) << 8  |
                                            (uint32_t)(b * 51));
    while (idx < 256) graphics_set_palette(idx++, 0);
}

void graphics_set_mode(const enum graphics_mode_t mode) {
    // Derived once per switch into composite rather than per scanline;
    // it is floating point and the renderer runs in an interrupt.
    if (mode == COMPOSITE_160x200x16) build_composite_palette();
    if (mode == VGA_320x200x256 && graphics_mode != VGA_320x200x256)
        build_vga_default_palette();

    /*
     * Only a Hercules can be wider than 640, and only sometimes: the
     * width is whatever the CRTC has been programmed for, sixteen
     * pixels a character: 45 is the stock 720, and Planet X3 asks for 40
     * and needs no retiming at all.
     */
    const bool wide = (mode == HERC_720x348x2 || mode == HERC_720x348x2_90) &&
                      mc6845_mda.r.h_displayed * 16 > 640;
    apply_timing(wide ? &timing_720x480 : &timing_640x480);

    graphics_mode = mode;
}
