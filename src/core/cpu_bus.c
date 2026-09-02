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

/*
 * Freezing the 8086.
 *
 * The HOLD pin is strapped to ground on this board, which for a long time
 * I took to mean the CPU could not be stopped. It was the wrong pin to
 * look at. READY is ours: GP27 drives the D input of U2, and the read path
 * in i8086_bus.pio already parks the CPU in wait states with READY low
 * while it blocks on `out pins` waiting for core 1 to supply data. An IDE
 * sector read holds the machine that way for milliseconds on every boot.
 * Holding it for the length of a menu is the same mechanism, longer.
 *
 * Nothing objects to an arbitrarily long stall: an 8086 has no bus
 * timeout, and system memory is PSRAM refreshed by the QMI controller
 * rather than DRAM waiting on a refresh cycle. The clock keeps running,
 * which it must -- a real NMOS 8086 is dynamic and loses state below
 * 2 MHz, so stopping CLK is the one thing we cannot do.
 *
 * Why bother: core 1 reaches FatFs from these handlers to service the
 * hard disk, and core 0 reaches it for the drive menu. Rather than make
 * that concurrency safe, this removes it -- the guest is stopped for as
 * long as a menu is open, so there is only ever one core in the
 * filesystem.
 */
volatile bool bus_pause_req;
volatile bool bus_paused;

/*
 * Every bus cycle the 8086 completes.
 *
 * A liveness signal that costs one increment. "Is the CPU actually
 * running?" turned out to be surprisingly hard to answer from outside --
 * the BIOS tick at 0040:006C is seeded once and never advanced, and an
 * idle DOS prompt writes nothing to the screen -- so a stopped machine
 * and an idle one looked identical. Read it twice over SWD and the
 * difference is unambiguous.
 */
volatile uint32_t bus_cycles;

__force_inline static void bus_pause_point(void) {
    if (likely(!bus_pause_req)) return;

    // Reached before this cycle is serviced, so core 1 is parked outside
    // FatFs, not part-way through a sector. On a read the PIO is still
    // holding READY low and the CPU simply waits; on a write the cycle has
    // already completed and the CPU stalls on its next one instead.
    bus_paused = true;
    while (bus_pause_req) tight_loop_contents();
    bus_paused = false;
}

void bus_pause(void) {
    bus_pause_req = true;

    /*
     * Wait for core 1 to confirm it has parked. The timeout is not
     * belt-and-braces: a guest sitting in HLT drives no bus cycles at
     * all, so the acknowledgement would never come. That case is safe to
     * proceed into for the same reason it cannot acknowledge -- a halted
     * CPU is not in the filesystem either.
     */
    const absolute_time_t deadline = make_timeout_time_ms(50);
    while (!bus_paused && absolute_time_diff_us(get_absolute_time(), deadline) > 0)
        tight_loop_contents();
}

void bus_resume(void) { bus_pause_req = false; }

void __time_critical_func(bus_write_handler)() {
    bus_pause_point();
    bus_cycles++;

    const uint32_t bus_state = BUS_CTRL_PIO->rxf[BUS_CTRL_SM];
    const uint16_t data = BUS_CTRL_PIO->rxf[BUS_CTRL_SM];

    i8086_write(bus_state & 0xFFFFF, data, bus_state & MIO, bus_state & BHE);

    pio_interrupt_clear(BUS_CTRL_PIO, 0);
}

void __time_critical_func(bus_read_handler)() {
    bus_pause_point();
    bus_cycles++;

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
