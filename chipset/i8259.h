#pragma once
#include "state.h"
extern i8259_s i8259;

__force_inline static uint8_t i8259_read(const uint16_t port_number) {
    switch (port_number) {
        case 0x20:
            return i8259.register_read_mode ? i8259.in_service_register : i8259.interrupt_request_register;
        case 0x21: //read mask register
            return i8259.interrupt_mask_register;
    }
}

__force_inline static void i8259_write(const uint16_t port_number, const uint8_t data) {
    switch (port_number) {
        case 0x20:
            if (data & 0x10) {
                //ICW1
                i8259.interrupt_mask_register = 0x00;
                i8259.initialization_command_words_1 = data;
                i8259.initialization_command_word_step = 2;
                i8259.register_read_mode = 0;
            } else if ((data & 0x08) == 0) {
                //OCW2
                //                i8259.ocw[2] = register_value;
                switch (data & 0xE0) {
                    case 0x20: //non-specific EOI
                        if (i8259.in_service_register) {
                            // __builtin_ctz возвращает количество нулей справа (индекс первого установленного бита)
                            const uint8_t highest_prio_isr = __builtin_ctz(i8259.in_service_register);
                            i8259.in_service_register &= ~(1 << highest_prio_isr);
                        }
                        break;
                    case 0x60: //specific EOI
                        i8259.in_service_register &= ~(1 << (data & 0x07)); // Только ISR!
                        break;
                    default: //other

                        break;
                }
            } else {
                //OCW3
                if (data & 0x08) { // Bit 3 должен быть 1 для OCW3
                    if (data & 0x02) { // Bit 1 = RR (Read Register)
                        // Bit 0 = RIS (0=IRR, 1=ISR)
                        i8259.register_read_mode = data & 1;
                    }
                }
            }
            break;
        case 0x21:
            switch (i8259.initialization_command_word_step) {
                case 2: //ICW2
                    // i8259.initialization_command_words[2] = register_value;
                    i8259.interrupt_vector_offset = data & 0xF8;
                    if (i8259.initialization_command_words_1 & 0x02) {
                        i8259.initialization_command_word_step = 4;
                    } else {
                        i8259.initialization_command_word_step = 3;
                    }
                    break;
                case 3: //ICW3
                    // i8259.initialization_command_words[3] = register_value;
                    if (i8259.initialization_command_words_1 & 0x01) {
                        i8259.initialization_command_word_step = 4;
                    } else {
                        i8259.initialization_command_word_step = 5; //done with ICWs
                    }
                    break;
                case 4: //ICW4
                    // i8259.initialization_command_words[4] = register_value;
                    i8259.initialization_command_word_step = 5; //done with ICWs
                    break;
                case 5: //just set IMR value now
                    i8259.interrupt_mask_register = data;
                    break;
            }
            break;
    }
}

__force_inline static uint8_t i8259_get_pending_irqs() {
    // Возвращает битовую маску IRQ, которые запрошены, но не замаскированы
    return i8259.interrupt_request_register & ~i8259.interrupt_mask_register;
}

__force_inline static void i8259_interrupt(const uint8_t irq) {
    // Устанавливаем бит в IRR если IRQ
    i8259.interrupt_request_register |= 1 << irq;
}

__force_inline static uint8_t i8259_nextirq() {
    const uint8_t pending = i8259_get_pending_irqs();
    if (!pending) return 0;

    // Находим IRQ с наивысшим приоритетом (наименьший номер)
    const uint8_t irq = __builtin_ctz(pending);

    // Перемещаем прерывание из IRR в ISR
    i8259.interrupt_request_register &=  ~(1 << irq);
    i8259.in_service_register |= 1 << irq;

    // Возвращаем вектор прерывания (base + IRQ)
    return i8259.interrupt_vector_offset + irq;
}
