/* Intel 8272A Floppy Disk Controller (упрощенная эмуляция для BIOS) */
#pragma once


#include "i8259.h"

// #define DEBUG_I8272
#if defined(DEBUG_I8272)
#define debug_log(...) printf(__VA_ARGS__)
#else
#define debug_log(...)
#endif

extern uint8_t FLOPPY[];
extern i8272_s i8272;

#define FDD_CYLINDERS 80
#define FDD_HEADS 2
#define FDD_SECTORS_PER_TRACK 18
#define FDD_SECTOR_SIZE 512

#define FDC_PORT_DOR  0x3F2
#define FDC_PORT_MSR  0x3F4
#define FDC_PORT_DATA 0x3F5


#define FDC_CMD_SPECIFY  0x03
#define FDC_CMD_SENSE_DRIVE_STATUS 0x04
#define FDC_CMD_WRITE_DATA  0x05
#define FDC_CMD_READ_DATA  0x06
#define FDC_CMD_RECALIBRATE  0x07
#define FDC_CMD_SENSE_INTERRUPT_STATUS  0x08
#define FDC_CMD_READ_ID  0x0A
#define FDC_CMD_SEEK  0x0F

__force_inline static void i8272_irq(void) {
    // if interrupts enabled
    if (i8272.DOR & (1 << 3)) {
        i8259_interrupt(6);
        debug_log("FDC: IRQ6 generated\n");
    }
}

__force_inline static uint8_t i8272_readport(const uint16_t port_number) {
    switch (port_number) {
        case FDC_PORT_MSR: // floppy main status
        {
            // no drives busy, dma-mode
            return 0 |
                   (i8272.result_count ? 1 << 4 : 0) | // fdc busy if there's a result to read
                   (i8272.result_count ? 1 << 6 : 0) | // fdc->cpu if we have result data else cpu->fdc
                   1 << 7; // ready
        }
        case FDC_PORT_DATA: // floppy command/data
        {
            if (i8272.result_count) {
                const auto r = i8272.result[i8272.result_index++];

                // end of a result
                if (i8272.result_index == i8272.result_count)
                    i8272.result_count = 0;

                return r;
            }

            return 0xFF;
        }
    }
}

__force_inline static void i8272_writeport(const uint16_t port_number, const uint8_t data) {
    switch (port_number) {
        case FDC_PORT_DOR: // floppy digital output
        {
            const auto dor_changed_bits = i8272.DOR ^ data;

            if ((dor_changed_bits & (1 << 2)) && (data & (1 << 2))) {
                i8272_irq();

                i8272.check_drives_mask = 0xF; // all of them
            }
            i8272.DOR = data;
            break;
        }

        case FDC_PORT_DATA: // floppy command/data
        {
            if (i8272.command_length == 0) {
                i8272.command[0] = data;
                auto const cmd = data & 0x1F;
                // printf("FCD = %02X\n", data);
                if (cmd == FDC_CMD_SPECIFY) // specify
                    i8272.command_length = 3;
                else if (cmd == FDC_CMD_SENSE_DRIVE_STATUS) // sense drive status
                    i8272.command_length = 2;
                else if (cmd == FDC_CMD_WRITE_DATA) // write
                    i8272.command_length = 9;
                else if (cmd == FDC_CMD_READ_DATA) // read
                    i8272.command_length = 9;
                else if (cmd == FDC_CMD_RECALIBRATE) // recalibrate
                    i8272.command_length = 2;
                else if (cmd == FDC_CMD_SENSE_INTERRUPT_STATUS) // sense interrupt status
                    i8272.command_length = 1;
                else if (cmd == FDC_CMD_READ_ID) // read id
                    i8272.command_length = 2;
                else if (cmd == FDC_CMD_SEEK)
                    i8272.command_length = 3;
                else {
                    // return an error
                    i8272.response[0] = 1 << 7; // invalid command
                    i8272.result_count = 1;
                    i8272.result[0] = i8272.response[0];
                    i8272.result_index = 0;
                }

                if (i8272.command_length)
                    i8272.command_index = 1;
            } else
                i8272.command[i8272.command_index++] = data;

            if (i8272.command_length && i8272.command_index == i8272.command_length) {
                // got full command
                auto const cmd = i8272.command[0] & 0x1F;
                if (cmd == FDC_CMD_READ_DATA) // read
                {
                    // multitrack mfm skip
                    [[maybe_unused]] bool multiTrack = i8272.command[0] & (1 << 7);
                    [[maybe_unused]] bool mfm = i8272.command[0] & (1 << 6);
                    // bool skipDeleted = command[0] & (1 << 5);

                    const int drive = i8272.command[1] & 3;
                    const int head = (i8272.command[1] >> 2) & 1;

                    const auto cylinder = i8272.command[2];
                    [[maybe_unused]] auto headAgain = i8272.command[3];
                    const auto sector = i8272.command[4];
                    const auto number = i8272.command[5];
                    const auto endOfTrack = i8272.command[6];
                    //auto gapLength = command[7];
                    //auto dataLength = command[8];

                    assert(head == headAgain);
                    assert(number == 2);
                    assert(multiTrack);
                    assert(mfm);

                    //auto sectorSize = 128 << number;

                    // У нас две дискетки: drive 0 и drive 1
                    const bool failed = drive > 1;

                    i8272.response[0] = drive | head << 2;

                    if (failed)
                        i8272.response[0] |= 1 << 6;

                    i8272.result_count = 7;
                    i8272.result[0] = i8272.response[0];
                    i8272.result[1] = i8272.response[1];
                    i8272.result[2] = i8272.response[2];
                    i8272.result[3] = cylinder;
                    i8272.result[4] = head;
                    i8272.result[5] = sector;
                    i8272.result[6] = number;

                    // printf("FDC: read sector %d\n", record); //
                    if (!failed) {
                        // Запускаем асинхронную DMA передачу на канале 2
                        // IRQ6 будет сгенерирован автоматически при завершении передачи
                        // const uint32_t size = endOfTrack * FDD_SECTOR_SIZE;
                        const uint32_t offset = ((cylinder * FDD_HEADS + head) * FDD_SECTORS_PER_TRACK + (sector - 1)) * FDD_SECTOR_SIZE;
                        dma_start_transfer(2, 0x01, &FLOPPY, offset, drive, 6);
                    } else {
                        // При ошибке генерируем IRQ сразу
                        i8272_irq();
                    }
                } else if (cmd == FDC_CMD_SENSE_INTERRUPT_STATUS) // sense interrupt status
                {
                    if (i8272.check_drives_mask) {
                        const int drive = __builtin_ctz(i8272.check_drives_mask);
                        i8272.check_drives_mask &= i8272.check_drives_mask - 1; // clear bit
                        i8272.result[0] = 0xC0 | drive;
                    } else {
                        i8272.result[0] = i8272.response[0];
                    }

                    i8272.result[1] = i8272.presentCylinder[i8272.response[0] & 3];
                    i8272.result_count = 2;
                } else if (cmd == FDC_CMD_SEEK) // seek
                {
                    const int drive = i8272.command[1] & 3;
                    // int head = (command[1] >> 2) & 1;
                    const auto cylinder = i8272.command[2];

                    i8272.response[0] = drive;
                    if (drive > 1)
                        i8272.response[0] |= 1 << 6 | 1 << 4; // abnormal termination/equipment check
                    else {
                        i8272.presentCylinder[drive] = cylinder;

                        // set seek end
                        i8272.response[0] |= 1 << 5;
                    }

                    i8272_irq();
                } else if (cmd == FDC_CMD_WRITE_DATA) // write
                {
                    [[maybe_unused]] bool multiTrack = i8272.command[0] & (1 << 7);
                    [[maybe_unused]] bool mfm = i8272.command[0] & (1 << 6);
                    // bool skipDeleted = command[0] & (1 << 5);

                    int drive = i8272.command[1] & 3;
                    int head = (i8272.command[1] >> 2) & 1;

                    auto cylinder = i8272.command[2];
                    [[maybe_unused]] auto headAgain = i8272.command[3];
                    auto sector = i8272.command[4];
                    [[maybe_unused]] auto number = i8272.command[5];
                    //auto endOfTrack = command[6];
                    //auto gapLength = command[7];
                    //auto dataLength = command[8];

                    assert(head == headAgain);
                    assert(number == 2);
                    assert(multiTrack);
                    assert(mfm);

                    // prepare for write
                    const bool failed = drive > 1;

                    i8272.response[0] = drive | head << 2;

                    if (failed)
                        i8272.response[0] |= 1 << 6;

                    i8272.result_count = 7;
                    i8272.result[0] = i8272.response[0];
                    i8272.result[1] = i8272.response[1];
                    i8272.result[2] = i8272.response[2];
                    i8272.result[3] = cylinder;
                    i8272.result[4] = head;
                    i8272.result[5] = sector;
                    i8272.result[6] = number;

                    if (!failed) {
                        // Запускаем асинхронную DMA передачу на канале 2
                        // IRQ6 будет сгенерирован автоматически при завершении передачи
                        // const uint32_t size = endOfTrack * FDD_SECTOR_SIZE;
                        const uint32_t offset = ((cylinder * FDD_HEADS + head) * FDD_SECTORS_PER_TRACK + (sector - 1)) * FDD_SECTOR_SIZE;
                        dma_start_transfer(2, 0x11, &FLOPPY, offset, drive, 6);
                    } else {
                        // При ошибке генерируем IRQ сразу
                        i8272_irq();
                    }
                } else if (cmd == FDC_CMD_SPECIFY) // specify
                {
                    // auto stepRateTime = command[1] >> 4;
                    // auto headUnloadTime = command[1] & 0xF;
                    // auto headLoadTime = command[2] >> 1;
                    // bool nonDMA = command[2] & 1;
                } else if (cmd == FDC_CMD_SENSE_DRIVE_STATUS) // sense drive status
                {
                    const int drive = i8272.command[1] & 3;
                    // int head = (command[1] >> 2) & 1;

                    const bool isTrack_0 = i8272.presentCylinder[drive] == 0;

                    i8272.result_count = 1;
                    i8272.result[0] = (isTrack_0 ? 1 << 4 : 0) | 1 << 5 /*ready*/;
                } else if (cmd == FDC_CMD_RECALIBRATE) // recalibrate
                {
                    const int drive = i8272.command[1] & 3;

                    i8272.response[0] = drive;
                    if (drive > 1)
                        i8272.response[0] |= 1 << 6 | 1 << 4; // abnormal termination/equipment check
                    else {
                        i8272.presentCylinder[drive] = 0;
                        i8272.response[0] |= 1 << 5; // set seek end
                    }

                    i8272_irq();
                } else if (cmd == FDC_CMD_READ_ID) // read id
                {
                    [[maybe_unused]] bool mfm = i8272.command[0] & (1 << 6);

                    const int drive = i8272.command[1] & 3;
                    const int head = (i8272.command[1] >> 2) & 1;

                    assert(mfm);

                    i8272.response[0] = drive | head << 2;

                    i8272.result_count = 7;
                    i8272.result[0] = i8272.response[0];
                    i8272.result[1] = i8272.response[1];
                    i8272.result[2] = i8272.response[2];
                    i8272.result[3] = i8272.presentCylinder[drive];
                    i8272.result[4] = head;
                    i8272.result[5] = 1;
                    i8272.result[6] = 2; // ?

                    i8272_irq();
                }


                i8272.command_length = 0;
                i8272.result_index = 0;
            }

            break;
        }
    }
}

#if defined(DEBUG_I8272)
#undef debug_log
#endif
