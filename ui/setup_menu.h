#pragma once
#include <stdint.h>
#include <stdbool.h>

// Типы элементов меню
enum menu_type_e {
    NONE,   // Разделитель/заголовок
    ARRAY,  // Выбор из массива значений (LEFT/RIGHT)
    STRING, // Строковый ввод (пока не реализован)
    EXIT,   // Выход из меню
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
    const char* text;           // Текст пункта меню (может содержать %s для ARRAY/STRING)
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
#define SETTINGS_VERSION 1

// Структура настроек проекта
typedef struct {
    uint16_t version;        // Версия структуры настроек
    uint8_t tandy_enabled;  // 0 = NO, 1 = YES
    uint8_t cpu_freq_index; // 0 = 1MHz, 1 = 4.75MHz, 2 = 6MHz
    char fda[256];           // Floppy #1 filename (увеличено для длинных путей)
    char fdb[256];           // Floppy #2 filename
    char hdd[256];           // HDD filename
} settings_s;

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
