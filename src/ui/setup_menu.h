#pragma once
#include <stdint.h>
#include <stdbool.h>

// Типы элементов меню
enum menu_type_e {
    NONE,   // Разделитель/заголовок
    ARRAY,  // Выбор из массива значений (LEFT/RIGHT)
    STRING, // Строковый ввод (пока не реализован)
    EXIT,   // Выход из меню
    ACTION, // ENTER runs the callback; `value` is a string to show
};

/* ----------------- menu drawing ----------------- */
static const char *footers[] = {
    [NONE] = "UP/DOWN: Navigate  ESC: Exit without saving",
    [ARRAY] = "ENTER/LEFT/RIGHT: Change  UP/DOWN: Navigate  ESC: Exit without saving",
    [STRING] = "ENTER: Browse  UP/DOWN: Navigate  ESC: Exit without saving",
    [EXIT] = "ENTER: Save and Exit  ESC: Exit without saving"
};

// Callback функция для пунктов меню (может быть nullptr)
typedef bool (*menu_callback_t)();

// Структура элемента меню
typedef struct __attribute__((__packed__)) {
    const char* text;           // The label. Plain text: the value is drawn
                                // separately, right-aligned, so the two
                                // columns line up without every label
                                // carrying its own hand-counted padding.
    enum menu_type_e type;      // Тип элемента
    void* value;                // Указатель на переменную (для ARRAY - uint8_t*, для STRING - char*)
    menu_callback_t callback;   // Callback функция (может быть nullptr)
    uint32_t max_value;         // Максимальное значение для ARRAY или max длина для STRING
    union {
        char value_list[10][20];  // Список строковых значений для ARRAY
        struct {
            uint8_t fg_color;     // Цвет текста для NONE (0-15)
            uint8_t bg_color;     // Цвет фона для NONE (0-15)
        } colors;
    };
} MenuItem;

// Версия структуры настроек (увеличивать при изменении структуры)
/*
 * 3, because `hercules` was added to the struct below without this being
 * raised. A saved file from before then loads one byte short: `hercules`
 * takes the first character of the drive path, '/' -- which is 47, and
 * therefore true. Hercules switched itself on for anyone with an older
 * config, took 32K of memory, and pointed its framebuffer at the text
 * page. Every field after it was off by one as well.
 */
#define SETTINGS_VERSION 3

// Структура настроек проекта
typedef struct {
    uint16_t version;        // Версия структуры настроек
    uint8_t tandy_enabled;  // 0 = NO, 1 = YES
    uint8_t cpu_freq_index; // 0 = 1MHz, 1 = 4.75MHz, 2 = 6MHz

    /*
     * Which monitor mode 6 is being watched on.
     *   0 = Auto, 1 = RGB, 2 = Composite.
     *
     * Auto follows the colour burst, which is how software actually asks.
     * The BIOS sets mode 6 with bit 2 of the mode register high, meaning
     * burst off; a program that wants artifact colour writes it low.
     * Planet X3 does exactly that -- 0x1A where the BIOS left 0x1E -- and
     * a composite monitor would then show colour. Following the bit is
     * the faithful behaviour and needs no configuration.
     *
     * The forced settings stay because the bit says what the card is
     * emitting, not what is plugged into it.
     */
    uint8_t composite;

    /*
     * Hercules, 0 = no, 1 = yes.
     *
     * A boot-time choice because it costs memory: the framebuffer sits at
     * 0xB0000, inside the 736K this machine normally offers, so enabling
     * it leaves the guest 704K. Deciding at boot means the BIOS counts
     * the smaller figure and DOS never sees memory disappear underneath
     * it -- which is exactly what would happen if a program could claim
     * the range after the count had already been taken.
     *
     * The card is then still only selected when software asks for it
     * through the mode register.
     */
    uint8_t hercules;
    char fda[256];           // Floppy #1 filename (увеличено для длинных путей)
    char fdb[256];           // Floppy #2 filename
    char hdd[256];           // HDD filename
} settings_s;

// settings.composite
#define CGA_MONITOR_AUTO      0
#define CGA_MONITOR_RGB       1
#define CGA_MONITOR_COMPOSITE 2

extern settings_s settings;

// Файловый браузер для выбора образов дисков
// Возвращает true если файл выбран, false при ESC
bool file_browser(char* selected_path, uint8_t max_path_len, const char* filter);

// Сохранение настроек на SD карту
bool save_settings(void);

// Загрузка настроек с SD карты
bool load_settings(void);

// Главная функция SETUP меню
void setup_menu(void);
