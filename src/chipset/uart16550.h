#pragma once
#include "state.h"
#include "hid_app.h"  // Для is_usb_mouse_connected()
#include "i8259.h"  // Для генерации IRQ4 (COM1)

// ============================================================================
// Intel 16550 UART (COM1) - Minimal Emulation for DOS CTTY + Serial Mouse
// ============================================================================
// Порты: 0x3F8-0x3FF (COM1)
//
// Регистры:
// 0x3F8 (DLAB=0): RBR (Read) / THR (Write) - Receive/Transmit Buffer
// 0x3F8 (DLAB=1): DLL - Divisor Latch Low
// 0x3F9 (DLAB=0): IER - Interrupt Enable Register
// 0x3F9 (DLAB=1): DLH - Divisor Latch High
// 0x3FA: IIR (Read) / FCR (Write) - Interrupt ID / FIFO Control
// 0x3FB: LCR - Line Control Register (bit 7 = DLAB)
// 0x3FC: MCR - Modem Control Register
// 0x3FD: LSR - Line Status Register
// 0x3FE: MSR - Modem Status Register
// 0x3FF: SPR - Scratch Register (not implemented)
//
// Поддержка прерываний:
// - IRQ4 (COM1) генерируется через i8259_interrupt(4) при получении данных
// - IER bit 0 (0x01) разрешает RDA (Received Data Available) interrupt
// - IIR возвращает 0x04 (RX_DATA) при наличии данных в FIFO
// ============================================================================

extern uart_16550_s uart;

// ============================================================================
// LSR (Line Status Register) bits - 0x3FD
// ============================================================================
#define LSR_DATA_READY      0x01  // Bit 0: Data ready in RBR
#define LSR_OVERRUN_ERROR   0x02  // Bit 1: Overrun error
#define LSR_PARITY_ERROR    0x04  // Bit 2: Parity error
#define LSR_FRAMING_ERROR   0x08  // Bit 3: Framing error
#define LSR_BREAK_INTERRUPT 0x10  // Bit 4: Break interrupt
#define LSR_THR_EMPTY       0x20  // Bit 5: Transmit Holding Register empty
#define LSR_TRANSMITTER_EMPTY 0x40 // Bit 6: Transmitter empty (TSR + THR)

// ============================================================================
// IIR (Interrupt Identification Register) bits - 0x3FA
// ============================================================================
#define IIR_NO_INTERRUPT    0x01  // Bit 0: No interrupt pending
#define IIR_MODEM_STATUS    0x00  // Priority 4 (lowest)
#define IIR_THR_EMPTY       0x02  // Priority 3
#define IIR_RX_DATA         0x04  // Priority 2
#define IIR_LINE_STATUS     0x06  // Priority 1 (highest)

// ============================================================================
// uart_write_byte - Запись байта в UART RBR (для эмуляции входящих данных)
// Используется для Microsoft Serial Mouse protocol
// ============================================================================
// How many bytes could be pushed right now without overflowing the ring.
// The mouse packet coalescer in drivers/usbhid/hid_app.c checks this
// before committing to a packet, so it can hold the delta back rather
// than let uart_write_byte() drop the tail of one three-byte packet and
// leave the guest's frame sync one byte short forever after.
__force_inline static uint8_t uart_rx_room(void) {
    return (uart.rx_tail - uart.rx_head - 1) & 0x0F;
}

/*
 * Returns true if the byte was queued, false if the FIFO had no room and
 * it was dropped -- silence used to be the whole story here, which was
 * fine for the identification byte this was written for (one byte, and
 * losing it just means the driver's own timeout retries). A mouse
 * packet's caller needs to tell the two apart: dropping the tail of a
 * three-byte packet is not silent to the guest, it desyncs its frame
 * sync until the next byte happens to look like a new one.
 */
__force_inline static bool uart_write_byte(const uint8_t data) {
    // Вычисляем следующую позицию head
    const uint8_t next_head = (uart.rx_head + 1) & 0x0F;

    // Проверяем, не переполнен ли FIFO
    if (next_head != uart.rx_tail) {
        // Есть место - записываем байт
        uart.rx_fifo[uart.rx_head] = data;
        uart.rx_head = next_head;
        uart.data_ready = true; // Устанавливаем флаг "данные доступны"

        // Генерируем IRQ4 (COM1) если прерывания разрешены
        // IER bit 0 (0x01) = RDA (Received Data Available interrupt enable)
        if (uart.ier & 0x01) {
            i8259_interrupt(4);  // COM1 = IRQ4
        }
        return true;
    }
    return false;
    // Если FIFO переполнен - молча игнорируем (эмуляция overrun)
}


// ============================================================================
// uart_read - Чтение регистров UART (порты 0x3F8-0x3FF)
// ============================================================================
__force_inline static uint8_t uart_read(const uint32_t port) {
    switch (port) {
        case 0x3F8: {
            // RBR/DLL - Receive Buffer Register or Divisor Latch Low
            if (uart.lcr & 0x80) {
                // DLAB=1: возвращаем младший байт делителя
                return uart.divisor & 0xFF;
            }
            // DLAB=0: читаем байт из FIFO
            if (uart.rx_tail != uart.rx_head) {
                // Есть данные в FIFO - извлекаем
                const uint8_t data = uart.rx_fifo[uart.rx_tail];
                uart.rx_tail = (uart.rx_tail + 1) & 0x0F; // Кольцевой буфер (16 элементов)

                // Обновляем data_ready: если FIFO опустел, сбрасываем флаг
                uart.data_ready = (uart.rx_tail != uart.rx_head);

                /*
                 * A real 8250/16450 -- what a serial mouse driver is
                 * written for -- interrupts once *per byte*: RBR read
                 * clears that byte's condition, and if another byte is
                 * already sitting there, the interrupt line is asserted
                 * again immediately. uart_write_byte() only calls
                 * i8259_interrupt() at write time, so three bytes queued
                 * together (one packet, written back-to-back in
                 * mouse_flush()) OR three or four bytes' worth of one
                 * IRR bit produce exactly one request no matter how many
                 * bytes arrived, because OR-ing the same bit twice is a
                 * no-op. A driver that reads one byte, sends EOI and
                 * returns -- which is exactly what a real UART trains it
                 * to do -- then never learns the other bytes are there:
                 * nothing calls i8259_interrupt(4) again until the next
                 * write, and by then the FIFO has more unread bytes
                 * than the write freed up. That backlog only grows,
                 * eventually blocking uart_rx_room() forever -- exactly
                 * the "moves once, then never again" symptom this was
                 * written to fix.
                 *
                 * Re-raising here for whatever is left closes exactly
                 * that gap: a driver that behaves like the hardware it
                 * was written for gets interrupted once per remaining
                 * byte, the same as that hardware would.
                 */
                if (uart.data_ready && (uart.ier & 0x01)) {
                    i8259_interrupt(4);
                }
                return data;
            }
            // FIFO пуст - возвращаем старое значение rbr (для совместимости)
            uart.data_ready = false;
            return uart.rbr;
        }

        case 0x3F9: {
            // IER/DLH - Interrupt Enable Register or Divisor Latch High
            if (uart.lcr & 0x80) {
                // DLAB=1: возвращаем старший байт делителя
                return (uart.divisor >> 8) & 0xFF;
            }
            // DLAB=0: возвращаем IER
            return uart.ier;
        }

        case 0x3FA: {
            // IIR - Interrupt Identification Register
            // Проверяем наличие данных в FIFO и состояние IER
            if (uart.data_ready && (uart.ier & 0x01)) {
                // Есть данные и прерывания разрешены
                return IIR_RX_DATA;  // 0x04 - Received Data Available
            }
            return IIR_NO_INTERRUPT;  // 0x01 - No interrupt pending
        }

        case 0x3FB: {
            // LCR - Line Control Register
            return uart.lcr;
        }

        case 0x3FC: {
            // MCR - Modem Control Register
            return uart.mcr;
        }

        case 0x3FD: {
            // LSR - Line Status Register
            // Критичные биты для DOS:
            // Bit 0: Data Ready (есть данные для чтения)
            // Bit 5: THR Empty (можно писать)
            // Bit 6: Transmitter Empty (полностью свободен)
            uint8_t lsr = LSR_THR_EMPTY | LSR_TRANSMITTER_EMPTY; // Всегда готов к передаче
            if (uart.data_ready) {
                lsr |= LSR_DATA_READY; // Есть данные для чтения
            }
            return lsr;
        }

        case 0x3FE: {
            // MSR - Modem Status Register
            // Эмулируем "все сигналы активны" для простоты
            return 0xB0; // CTS=1, DSR=1, DCD=1
        }

        case 0x3FF: {
            // SPR - Scratch Register (не реализовано)
            return 0x00;
        }

        default:
            return 0xFF;
    }
}

// ============================================================================
// uart_write - Запись в регистры UART (порты 0x3F8-0x3FF)
// ============================================================================
__force_inline static void uart_write(const uint32_t port, const uint8_t data) {
    switch (port) {
        case 0x3F8: {
            // THR/DLL - Transmit Holding Register or Divisor Latch Low
            if (uart.lcr & 0x80) {
                // DLAB=1: устанавливаем младший байт делителя
                uart.divisor = (uart.divisor & 0xFF00) | data;
                return;
            }
            // DLAB=0: отправляем байт в USB терминал
            uart.thr = data;
            // putchar_raw(data); // Немедленный вывод в USB
            return;
        }

        case 0x3F9: {
            // IER/DLH - Interrupt Enable Register or Divisor Latch High
            if (uart.lcr & 0x80) {
                // DLAB=1: устанавливаем старший байт делителя
                uart.divisor = (uart.divisor & 0x00FF) | (data << 8);
                return;
            }
            // DLAB=0: устанавливаем IER
            uart.ier = data;
            return;
        }

        case 0x3FA: {
            // FCR - FIFO Control Register (write-only)
            // Для простоты игнорируем - работаем без FIFO
            return;
        }

        case 0x3FB: {
            // LCR - Line Control Register
            uart.lcr = data;
            return;
        }

        case 0x3FC: {
            // MCR - Modem Control Register
            // Биты: DTR (bit 0), RTS (bit 1), OUT1 (bit 2), OUT2 (bit 3), Loopback (bit 4)

            const uint8_t old_mcr = uart.mcr;
            uart.mcr = data;

            // Microsoft Serial Mouse: при установке RTS или DTR драйвер ожидает идентификатор
            // ВАЖНО: отправляем идентификатор ТОЛЬКО если реально подключена USB мышь
            const bool rts_rising = (!(old_mcr & 0x02)) && (data & 0x02);
            const bool dtr_rising = (!(old_mcr & 0x01)) && (data & 0x01);

            if (/*is_usb_mouse_connected() && */(rts_rising || dtr_rising)) {
                // Powering on: whatever was left in the FIFO predates this
                // driver's own init and is not what it is expecting to
                // read first. A real mouse's first byte on power-up is
                // 'M' with nothing ahead of it; give this one the same
                // clean start rather than let a stale byte from a prior
                // session bury the identifier the driver is waiting for.
                uart.rx_head    = 0;
                uart.rx_tail    = 0;
                uart.data_ready = false;

                // Отправляем идентификатор Microsoft Serial Mouse (2-button)
                // "M" = ASCII 0x4D
                uart_write_byte('M');
            }
            return;
        }

        case 0x3FD: {
            // LSR - Line Status Register (read-only, ignore writes)
            return;
        }

        case 0x3FE: {
            // MSR - Modem Status Register (read-only, ignore writes)
            return;
        }

        case 0x3FF: {
            // SPR - Scratch Register (не реализовано)
            return;
        }

        default:
            return;
    }
}

