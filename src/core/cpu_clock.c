#include "state.h"

#include <hardware/pwm.h>


void start_cpu_clock(uint32_t frequency_khz)
{
    // Calculate divider and wrap for best precision
    uint32_t div = 1;
    uint32_t wrap;

    while (div < 256) {
        wrap = (PICO_CLOCK_SPEED / (div * (frequency_khz * KHZ))) - 1;
        if (wrap <= 65535) break;
        div *= 10;  // Try 1, 10, 100, 1000
    }

    const uint32_t level = ((wrap + 1) * I8086_DUTY_CYCLE) / 100;

    gpio_set_function(CLOCK_PIN, GPIO_FUNC_PWM);

    const uint slice_num = pwm_gpio_to_slice_num(CLOCK_PIN);
    const uint channel = pwm_gpio_to_channel(CLOCK_PIN);

    pwm_config config = pwm_get_default_config();
    pwm_config_set_clkdiv_int(&config, div);
    pwm_config_set_wrap(&config, wrap);

    pwm_init(slice_num, &config, false);
    pwm_set_chan_level(slice_num, channel, level);
    pwm_set_enabled(slice_num, true);
}

void reset_cpu(void)
{
    /*
     * The serial mouse's handshake needs a clean modem-control register,
     * which this pin does not give it.
     *
     * Microsoft Serial Mouse identifies itself on a *rising* edge of DTR
     * or RTS: the driver asserts one, the port answers 'M', and that is
     * how CuteMouse knows a mouse is there at all. RESET_PIN restarts the
     * 8086, not this firmware's own state, so uart.mcr carries on holding
     * whatever the previous session's driver left it at -- both lines
     * already high, the steady state after a successful init. The next
     * session's driver raises them the same way it always does, finds no
     * edge to trigger on because they were never lowered first, gets no
     * 'M', and reports no mouse. Only a full power cycle, which does
     * reset this struct, ever saw the identification succeed.
     *
     * Clearing it here is what powering on real hardware would do to a
     * real UART's control lines, and it is what makes every boot behave
     * like the first one rather than like whatever the last session
     * happened to leave behind.
     */
    extern uart_16550_s uart;
    uart.mcr        = 0;
    uart.data_ready = false;
    uart.rx_head    = 0;
    uart.rx_tail    = 0;

    gpio_init(RESET_PIN);
    gpio_set_dir(RESET_PIN, GPIO_OUT);

    // Assert RESET (active LOW) for minimum 4 clock cycles (use 10 for safety)
    gpio_put(RESET_PIN, 1);
    busy_wait_ms(10);
    gpio_put(RESET_PIN, 0);

}


