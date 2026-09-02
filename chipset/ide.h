#pragma once

// Отладочный вывод HDD
//#define DEBUG_HDD
#if defined(DEBUG_HDD)
#define debug_log(...) printf(__VA_ARGS__)
#else
#define debug_log(...)
#endif

// Регистры
enum {
    IDE_DATA = 0,
    IDE_DATA_HIGH = 8,
    IDE_ERROR = 1, // Read
    IDE_FEATURES = 1, // Write
    IDE_SEC_COUNT = 2,
    IDE_LBA_LOW = 3,
    IDE_LBA_MID = 4,
    IDE_LBA_HIGH = 5,
    IDE_DEVICE = 6,
    IDE_STATUS = 7, // Read
    IDE_COMMAND = 7, // Write
};

// Флаги Статуса
#define IDE_STATUS_ERROR            0x01 // Error
#define IDE_STATUS_DATA_REQUEST     0x08 // Data Request (DRQ)
#define IDE_STATUS_SEEK_COMPLETE    0x10 // Drive Seek Complete (DSC)
#define IDE_STATUS_READY            0x40 // Drive Ready (DRDY)
#define IDE_STATUS_BUSY             0x80 // Busy (BSY)

extern ide_s ide;
constexpr uint8_t heads = 16;
constexpr uint8_t sectors = 63;

static inline void ide_set_busy() {
    ide.regs[IDE_STATUS] = IDE_STATUS_BUSY;
}

static inline void ide_set_drq() {
    ide.regs[IDE_STATUS] = IDE_STATUS_READY | IDE_STATUS_SEEK_COMPLETE | IDE_STATUS_DATA_REQUEST;
}

static inline void ide_set_ready() {
    ide.regs[IDE_STATUS] = IDE_STATUS_READY | IDE_STATUS_SEEK_COMPLETE;
}

static inline void ide_set_error(const uint8_t err) {
    ide.regs[IDE_ERROR] = err;
    ide.regs[IDE_STATUS] = IDE_STATUS_ERROR | IDE_STATUS_READY | IDE_STATUS_SEEK_COMPLETE;
}

static void ide_padstr(uint8_t *dst, const char *src, const int len) {
    for (int i = 0; i < len; i++) {
        dst[i ^ 1] = *src ? *src++ : ' '; // XOR 1 меняет Endianness (0->1, 1->0)
    }
}

static uint32_t ide_get_lba() {
    if (ide.regs[IDE_DEVICE] & 0x40) { // LBA mode (bit 6)
        uint32_t lba = ide.regs[IDE_LBA_LOW];
        lba |= ((uint32_t) ide.regs[IDE_LBA_MID] << 8);
        lba |= ((uint32_t) ide.regs[IDE_LBA_HIGH] << 16);
        lba |= ((uint32_t) (ide.regs[IDE_DEVICE] & 0x0F) << 24);
        return lba;
    }

    const uint8_t head = ide.regs[IDE_DEVICE] & 0x0F;
    const uint8_t sector = ide.regs[IDE_LBA_LOW];

    const uint16_t cylinder = ide.regs[IDE_LBA_MID] | ((uint16_t) ide.regs[IDE_LBA_HIGH] << 8);

    return ((uint32_t) cylinder * heads + head) * sectors + (sector - 1);
}

// Увеличить LBA регистры (low->mid->high->device[3:0]), учесть carry для LBA28
static void increment_lba_regs() {
    ide.regs[IDE_LBA_LOW]++;
    if (ide.regs[IDE_LBA_LOW] == 0) {
        ide.regs[IDE_LBA_MID]++;
        if (ide.regs[IDE_LBA_MID] == 0) {
            ide.regs[IDE_LBA_HIGH]++;
            if (ide.regs[IDE_LBA_HIGH] == 0) {
                // Переполнение LBA_HIGH - инкрементируем младшие 4 бита DEVICE для LBA28
                // Биты 7:4 DEVICE сохраняем (бит 6 = LBA mode flag, бит 4 = drive select)
                ide.regs[IDE_DEVICE] = (ide.regs[IDE_DEVICE] & 0xF0) |
                                       ((ide.regs[IDE_DEVICE] + 1) & 0x0F);
            }
        }
    }
}

// Функция чтения сектора с диска в секторный буфер; возвращает 0 при успехе или код ошибки
static int load_sector_from_disk(const uint32_t LBA) {
    if (!ide.disk_image) return -1;
    if (f_lseek(ide.disk_image, (FSIZE_t)LBA * 512) != FR_OK) return -2;
    UINT bytes_read = 0;
    if (f_read(ide.disk_image, ide.sector_buffer, 512, &bytes_read) != FR_OK) return -3;
    if (bytes_read < 512) memset(ide.sector_buffer + bytes_read, 0, 512 - bytes_read);
    return 0;
}

// Аналогично — записать буфер на диск (используется при завершении записи одного сектора)
static int write_sector_to_disk(const uint32_t LBA) {
    if (!ide.disk_image) return -1;
    if (f_lseek(ide.disk_image, (FSIZE_t)LBA * 512) != FR_OK) return -2;
    UINT bytes_written = 0;
    if (f_write(ide.disk_image, ide.sector_buffer, 512, &bytes_written) != FR_OK || bytes_written != 512) return -3;
    // if (f_sync(ide.disk_image) != FR_OK) return -4; // optional
    return 0;
}

// --- Реализация команд согласно ATA-1 спецификации ---
static void ide_identify() {
    // Очистить буфер 512 байт
    memset(ide.sector_buffer, 0, 512);
    uint16_t* identify = (uint16_t*)ide.sector_buffer;

    // Рассчитать геометрию диска
    uint32_t total_sectors = 0;
    if (ide.disk_image) total_sectors = (uint32_t)(f_size(ide.disk_image) / 512);

    uint32_t cylinders = (total_sectors / (heads * sectors));
    if (cylinders < 1) cylinders = 1;
    if (cylinders > 16383) cylinders = 16383;

    // Word 0: General configuration
    // Бит 6 = 1: Fixed disk (non-removable)
    // Бит 7 = 0: ATA device
    identify[0] = 0x0040;

    // Word 1-3: Default CHS geometry (obsolete but required for old BIOS)
    identify[1] = (uint16_t)cylinders;  // Cylinders
    identify[2] = 0;                     // Reserved
    identify[3] = heads;                 // Heads
    identify[4] = sectors * 512;         // Unformatted bytes per track (obsolete)
    identify[5] = 512;                   // Unformatted bytes per sector (obsolete)
    identify[6] = sectors;               // Sectors per track

    // Word 7-9: Reserved for CompactFlash
    identify[7] = 0;
    identify[8] = 0;
    identify[9] = 0;

    // Word 10-19: Serial number (20 ASCII characters, byte-swapped)
    ide_padstr(&ide.sector_buffer[20], "RP8086-XTIDE-001", 20);

    // Word 20-22: Buffer type/size (obsolete, set 0)
    identify[20] = 0;
    identify[21] = 0;
    identify[22] = 0;

    // Word 23-26: Firmware revision (8 ASCII characters, byte-swapped)
    ide_padstr(&ide.sector_buffer[46], "v1.0", 8);

    // Word 27-46: Model number (40 ASCII characters, byte-swapped)
    // Простое решение - использовать константу, т.к. settings может быть недоступен в header-only файле
    // TODO: передавать model_name через параметры или использовать глобальную переменную
    ide_padstr(&ide.sector_buffer[54], "HDD IMAGE ON SD", 40);

    // Word 47: Multiple sector transfer (bits 7:0 = max sectors per interrupt)
    // 0x8000 = valid, 0x0001 = max 1 sector (can be expanded to 0x80xx for multi-sector)
    identify[47] = 0x8001;

    // Word 48: Reserved (0 for ATA-1)
    identify[48] = 0;

    // Word 49: Capabilities
    // Бит 9 = 1: LBA supported
    // Бит 8 = 0: DMA not supported
    identify[49] = 0x0200;

    // Word 50: Reserved
    identify[50] = 0;

    // Word 51-52: PIO timing modes (obsolete, set 0)
    identify[51] = 0;
    identify[52] = 0;

    // Word 53: Field validity
    // Бит 0 = 1: Words 54-58 are valid
    // Бит 1 = 1: Words 64-70 are valid (для ATA-2+)
    identify[53] = 0x0001;

    // Word 54-56: Current CHS geometry (same as words 1,3,6 if not translated)
    identify[54] = (uint16_t)cylinders;  // Current cylinders
    identify[55] = heads;                 // Current heads
    identify[56] = sectors;               // Current sectors per track

    // Word 57-58: Current capacity in sectors (CHS mode)
    uint32_t chs_capacity = cylinders * heads * sectors;
    identify[57] = (uint16_t)(chs_capacity & 0xFFFF);        // Low word
    identify[58] = (uint16_t)((chs_capacity >> 16) & 0xFFFF); // High word

    // Word 59: Multiple sector setting (currently valid sector count per interrupt)
    // 0x0001 = 1 sector valid
    identify[59] = 0x0001;

    // Word 60-61: Total addressable sectors in LBA mode (little-endian 32-bit)
    identify[60] = (uint16_t)(total_sectors & 0xFFFF);        // Low word
    identify[61] = (uint16_t)((total_sectors >> 16) & 0xFFFF); // High word

    // Word 62-127: Reserved/vendor-specific (оставляем 0)

    ide_set_drq();
}

static void read_sector(void) {
    if (!ide.disk_image) {
        ide_set_error(0x04);
        return;
    }

    const uint8_t sectors_count = ide.regs[IDE_SEC_COUNT];
    const uint16_t total_sectors = (sectors_count == 0) ? 256 : sectors_count;
    const uint32_t LBA = ide_get_lba();

    // Проверка границ диска
    const uint32_t max_lba = (uint32_t)(f_size(ide.disk_image) / 512);
    if (LBA + total_sectors > max_lba) {
        debug_log("IDE: Read out of bounds! LBA=%lu+%u > %lu\n", (unsigned long)LBA, (unsigned)total_sectors, (unsigned long)max_lba);
        ide_set_error(0x10); // ID not found (sector not found)
        return;
    }

    const int result = load_sector_from_disk(LBA);
    if (result != 0) {
        debug_log("IDE: Read error LBA=%lu ret=%d\n", (unsigned long)LBA, result);
        ide_set_error(0x40);
        return;
    }

    ide.sectors_remaining = total_sectors;

    debug_log("IDE: cmd_read_sector - sectors_remaining=%u LBA=%lu\n", (unsigned)ide.sectors_remaining, (unsigned long)LBA);
    ide_set_drq();
}

static void write_sector(void) {
    if (!ide.disk_image) {
        ide_set_error(0x04);
        return;
    }

    const uint8_t sectors_count = ide.regs[IDE_SEC_COUNT];
    const uint16_t total_sectors = (sectors_count == 0) ? 256 : sectors_count;
    const uint32_t LBA = ide_get_lba();

    // Проверка границ диска
    const uint32_t max_lba = (uint32_t)(f_size(ide.disk_image) / 512);
    if (LBA + total_sectors > max_lba) {
        debug_log("IDE: Write out of bounds! LBA=%lu+%u > %lu\n", (unsigned long)LBA, (unsigned)total_sectors, (unsigned long)max_lba);
        ide_set_error(0x10); // ID not found (sector not found)
        return;
    }

    ide.sectors_remaining = total_sectors;
    debug_log("IDE: cmd_write_sector - sectors_remaining=%u\n", (unsigned)ide.sectors_remaining);
    ide_set_drq();
}

// READ VERIFY (без данных)
static void read_verify() {
    const uint32_t LBA = ide_get_lba();
    const uint8_t sectors_count = ide.regs[IDE_SEC_COUNT];
    const uint16_t total_sectors = (sectors_count == 0) ? 256 : sectors_count;

    debug_log("IDE: READ VERIFY LBA=%lu Count=%u\n", (unsigned long)LBA, (unsigned)total_sectors);

    if (ide.disk_image) {
        const uint32_t max_lba = (uint32_t) (f_size(ide.disk_image) / 512);
        if (LBA + total_sectors > max_lba) {
            debug_log("IDE: READ VERIFY out of bounds! LBA=%lu+%u > %lu\n", (unsigned long)LBA, (unsigned)total_sectors, (unsigned long)max_lba);
            ide_set_error(0x10);
            return;
        }
    }
    ide_set_ready();
}


// --- Главный обработчик команд ---
__force_inline static void handle_command(const uint8_t cmd) {
    ide.current_command = cmd;
    ide.buffer_index = 0;

    ide_set_busy();

    switch (cmd) {
        case 0xEC: ide_identify();
            break;

        case 0x20: read_sector();
            break;

        case 0x30: write_sector();
            break;

        case 0x40:
        case 0x41:
            read_verify();
            break;

        case 0x91:
        case 0xEF:
        case 0x70:
        case 0x10:
            ide_set_ready();
            break;

        default:
            printf("IDE: Unknown command 0x%02X - ABORTED\n", cmd);
            ide_set_error(0x04);
            break;
    }
}

__force_inline static uint8_t ide_read(uint16_t port_number) {
    port_number &= 0xf;

    switch (port_number) {
        case IDE_DATA: {
            const uint16_t word = *(const uint16_t *) &ide.sector_buffer[ide.buffer_index];
            ide.buffer_index += 2;
            ide.high_byte = (uint8_t) (word >> 8);

            if (ide.buffer_index >= 512) {
                ide.buffer_index = 0;

                if (ide.current_command == 0x20) {
                    // READ SECTORS
                    if (ide.sectors_remaining > 0) ide.sectors_remaining--;
                    debug_log("IDE: Multi-sector read - sectors_remaining=%u\n", (unsigned)ide.sectors_remaining);

                    if (ide.sectors_remaining > 0) {
                        increment_lba_regs();
                        const uint32_t LBA = ide_get_lba();
                        if (load_sector_from_disk(LBA) == 0) {
                            ide_set_drq();
                        } else {
                            debug_log("IDE: Multi-sector read error at LBA=%lu\n", (unsigned long)LBA);
                            ide_set_error(0x10);
                        }
                    } else {
                        debug_log("IDE: Multi-sector read complete\n");
                        ide_set_ready();
                    }
                } else {
                    // Для других команд просто снять DRQ
                    ide_set_ready();
                }
            }
            return (uint8_t) (word & 0xff);
        }
        case IDE_DATA_HIGH:
            return ide.high_byte;
        default:
            return ide.regs[port_number];
    }
}

__force_inline static void ide_write(uint16_t port_number, const uint8_t data) {
    port_number &= 0xf;

    switch (port_number) {
        case IDE_DATA: {
            *(uint16_t *) &ide.sector_buffer[ide.buffer_index] = data | (ide.high_byte << 8);
            ide.buffer_index += 2;

            if (ide.buffer_index >= 512) {
                const uint32_t LBA = ide_get_lba();
                debug_log("IDE: Buffer write complete - flushing to disk LBA=%lu\n", (unsigned long)LBA);
                ide.buffer_index = 0;

                if (ide.current_command == 0x30) {
                    const int result = write_sector_to_disk(LBA);
                    if (result != 0) {
                        debug_log("IDE: flush_buffer write error ret=%d\n", result);
                        ide_set_error(0x40);
                        return;
                    }

                    if (ide.sectors_remaining > 0) ide.sectors_remaining--;
                    debug_log("IDE: Multi-sector write - sectors_remaining=%u\n", (unsigned)ide.sectors_remaining);

                    if (ide.sectors_remaining > 0) {
                        increment_lba_regs();
                        ide_set_drq();
                    } else {
                        debug_log("IDE: Multi-sector write complete\n");
                        ide_set_ready();
                    }
                } else {
                    // Если это не команда записи — просто снять DRQ и установить READY
                    ide_set_ready();
                }
            }
            return;
        }
        case IDE_DATA_HIGH: {
            ide.high_byte = data;
            return;
        }
        case IDE_COMMAND:
            handle_command(data);
            return;
        case IDE_DEVICE:
            if (ide.disk_image)
                ide_set_ready();
            // fallthrough to store device reg as well
            // (if you need to mirror device write to regs, below will handle)
        default:
            ide.regs[port_number] = data;
    }
}