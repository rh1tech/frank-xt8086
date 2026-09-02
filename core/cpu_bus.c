#include "i8086_bus.pio.h"
#include "state.h"
#include "memory.h"
#include "ports.h"
#include "i8259.h"

// INTA pending IRQ vector (вынесено для минимизации memory access overhead)
static uint16_t irq_pending_vector = 0;

// ============================================================================
// Bus Read/Write Routing (маршрутизация между memory и ports)
// ============================================================================

__force_inline static uint16_t i8086_read(const uint32_t address, const bool is_memory_access, const bool bhe) {
    bus_trace(1);
    return is_memory_access ? memory_read(address  & 0xFFFFE) : port_read(address & 0xFFF, bhe);
}

__force_inline static void i8086_write(const uint32_t address, const uint16_t data,
                                        const bool is_memory_access, const bool bhe) {
    bus_trace(1);
    return is_memory_access ? memory_write(address, data, bhe) : port_write(address, data, bhe);
}

void __time_critical_func(bus_write_handler)() {
    const uint32_t bus_state = BUS_CTRL_PIO->rxf[BUS_CTRL_SM];
    const uint16_t data = BUS_CTRL_PIO->rxf[BUS_CTRL_SM];

    i8086_write(bus_state & 0xFFFFF, data, bus_state & MIO, bus_state & BHE);

    pio_interrupt_clear(BUS_CTRL_PIO, 0);
}

void __time_critical_func(bus_read_handler)() {
    // INTA cycle проверяем первым (более редкий, но высокоприоритетный)
    if (unlikely(pio_interrupt_get(BUS_CTRL_PIO, 3))) {
        pio_interrupt_clear(BUS_CTRL_PIO, 3);
        const uint8_t vector = i8259_nextirq();
        if (vector) {
            irq_pending_vector = 0xFF00 | vector;
        }
        return; // ← ВАЖНО: INTA не требует чтения данных
    }

    // IRQ1 - обычное чтение (без дополнительной проверки)
    const uint32_t bus_state = BUS_CTRL_PIO->rxf[BUS_CTRL_SM];

    if (unlikely(irq_pending_vector)) {
        BUS_CTRL_PIO->txf[BUS_CTRL_SM] = irq_pending_vector << 16 | 0x00FF;
        irq_pending_vector = 0;
    } else {
        BUS_CTRL_PIO->txf[BUS_CTRL_SM] = i8086_read(bus_state, bus_state & MIO, bus_state & BHE) << 16 | 0xFFFF;
    }

    pio_interrupt_clear(BUS_CTRL_PIO, 1);
}

void cpu_bus_init() {
    const uint pio_offset = pio_add_program(BUS_CTRL_PIO, &i8086_bus_program);
    i8086_bus_program_init(BUS_CTRL_PIO, BUS_CTRL_SM, pio_offset);

    pio_set_irq0_source_enabled(BUS_CTRL_PIO, pis_interrupt0, true);
    irq_set_exclusive_handler(WRITE_IRQ, bus_write_handler);
    irq_set_priority(WRITE_IRQ, PICO_HIGHEST_IRQ_PRIORITY);
    irq_set_enabled(WRITE_IRQ, true);

    pio_set_irq1_source_enabled(BUS_CTRL_PIO, pis_interrupt1, true);
    pio_set_irq1_source_enabled(BUS_CTRL_PIO, pis_interrupt3, true);
    irq_set_exclusive_handler(READ_IRQ, bus_read_handler);
    irq_set_priority(READ_IRQ, PICO_HIGHEST_IRQ_PRIORITY);
    irq_set_enabled(READ_IRQ, true);

    pio_sm_set_enabled(BUS_CTRL_PIO, BUS_CTRL_SM, true);
}
