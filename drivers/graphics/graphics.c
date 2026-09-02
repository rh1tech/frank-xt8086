#include "graphics.h"
#include <string.h>
#include <stdio.h>

#include "state.h"

static inline void draw_text_fast(const char *text, const uint32_t x, const uint32_t y, const uint8_t attribute)
{
    auto videoram = (uint16_t *)(VIDEORAM + (y * TEXTMODE_COLS + x) * 2);

    while (*text) {
        *videoram++ = attribute << 8 | *text++;
    }
}


void draw_text(const char *text, const uint32_t x, const uint32_t y, const uint8_t fgcolor, const uint8_t bgcolor) {
    draw_text_fast(text, x, y, bgcolor << 4 | fgcolor & 0xF);
}

void draw_window(const char *title, const char *footer, const uint32_t x, const uint32_t y, const uint32_t width, const uint32_t height)
{
    if (width < 3 || height < 3) return;

    constexpr uint8_t attr_frame = (1 << 4) | 11; // bg=1 fg=11
    constexpr uint8_t attr_title = (3 << 4) | 14; // bg=3 fg=14
    constexpr uint8_t attr_footer = (1 << 4) | 7;

    char buffer[TEXTMODE_COLS + 1];
    const uint32_t inner = width - 2;

    // ┌──────┐ Верхняя граница
    buffer[0] = 0xC9; // ╔
    memset(buffer + 1, 0xCD, inner);
    buffer[width - 1] = 0xBB; // ╗
    buffer[width] = 0;
    draw_text_fast(buffer, x, y, attr_frame);

    // └──────┘ Нижняя граница
    buffer[0] = 0xC8; // ╚
    memset(buffer + 1, 0xCD, inner);
    buffer[width - 1] = 0xBC; // ╝
    draw_text_fast(buffer, x, y + height - 1, attr_frame);

    // Вертикальные линии + пробелы внутри
    memset(buffer + 1, ' ', inner);
    buffer[0] = buffer[width - 1] = 0xBA; // ║
    buffer[width] = 0;

    for (uint32_t i = 1; i < height - 1; i++) {
        draw_text_fast(buffer, x, y + i, attr_frame);
    }

    // Заголовок
    if (title && *title) {
        snprintf(buffer, inner, " %s ", title);
        const size_t title_length = strlen(buffer);
        const uint32_t title_x = x + (width - title_length) / 2;
        draw_text_fast(buffer, title_x, y, attr_title);
    }

    // Футер
    if (footer && *footer) {
        snprintf(buffer, inner, " %s ", footer);
        const size_t footer_length = strlen(buffer);
        const uint32_t footer_x = x + (width - footer_length) / 2;
        draw_text_fast(buffer, footer_x, y + height - 1, attr_footer);
    }
}
