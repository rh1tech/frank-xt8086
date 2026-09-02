#pragma once
#include "state.h"
#include "hardware/pwm.h"

#define PIT_MODE_LATCHCOUNT  0
#define PIT_MODE_LOBYTE      1
#define PIT_MODE_HIBYTE      2
#define PIT_MODE_TOGGLE      3
#define PIT_FREQUENCY        1193182

extern i8253_s i8253;
extern i8259_s i8259;
extern uint8_t port61;
extern pwm_config pwm;

extern repeating_timer_t irq0_timer;

static bool irq0_timer_callback(struct repeating_timer *t) {
    const auto operating_mode = (uint8_t *) t->user_data;
    i8259_interrupt(0);
    return *operating_mode != 0;
}

__force_inline static uint16_t i8253_get_current_count(const i8253_channel_s *channel) {
    const uint32_t reload = channel->reload_value ? : 65536;
    const uint64_t now = get_absolute_time();

    const uint64_t ticks = ((now - channel->start_timestamp_us) * PIT_FREQUENCY) / 1000000ULL;

    if ((channel->operating_mode & 7) == 0) {
        if (!channel->active) return 0;
        if (ticks >= reload) return 0;
        return (uint16_t)(reload - ticks);
    }

    return (uint16_t) (reload - 1 - ticks % reload);
}

__force_inline static uint8_t i8253_read(const uint16_t port_number) {
    i8253_channel_s *channel = &i8253.channels[port_number & 3];

    const uint16_t value = i8253_get_current_count(channel);
    switch (channel->access_mode) {
        case PIT_MODE_LOBYTE:
            return value;
        case PIT_MODE_HIBYTE:
            return value >> 8;
        case PIT_MODE_TOGGLE:
            if (channel->byte_toggle == 0) {
                channel->byte_toggle ^= 1;
                return value;
            }

            channel->byte_toggle ^= 1;
            return value >> 8;
            break;
        default:
            return 0xFF;
    }
}

__force_inline static void i8253_write(const uint16_t port_number, const uint8_t data) {
    if (port_number <= 0x42) {
        const uint8_t channel_index = port_number & 3;
        // Запись данных в канал (порты 0x40-0x42)
        i8253_channel_s *channel = &i8253.channels[channel_index];

        if (channel->access_mode == PIT_MODE_LOBYTE) {
            channel->reload_value = data;
        } else if (channel->access_mode == PIT_MODE_HIBYTE) {
            channel->reload_value = ((uint16_t) data << 8);
        } else {
            // PIT_MODE_TOGGLE
            if (channel->byte_toggle == 0) {
                channel->reload_value = (channel->reload_value & 0xFF00) | data;
                channel->byte_toggle ^= 1;
                return;
            }
            channel->reload_value = (channel->reload_value & 0x00FF) | ((uint16_t) data << 8);
            channel->byte_toggle ^= 1;
        }

        channel->active = true;
        channel->start_timestamp_us = get_absolute_time();
        // Канал 0 управляет системным таймером
        if (channel_index == 0) {
            const uint32_t reload_value = channel->reload_value ? : 65536;
            const int64_t timer_interval = (uint32_t) (((uint64_t) reload_value * 1000000ULL) / PIT_FREQUENCY);
            i8259.interrupt_request_register &= ~(1 << 0);

            cancel_repeating_timer(&irq0_timer);

            add_repeating_timer_us(-timer_interval, irq0_timer_callback, &channel->operating_mode, &irq0_timer);

        } else if (channel_index == 2) {
            // PC speaker: PWM base ~3.97 MHz / reload = audible square wave.
            // Drive at 50 % duty (wrap/2) so the magnetic transducer gets
            // the full AC swing it needs — fixed 127 was effectively low
            // duty for typical DOS reload values (~1193 → ~10 %).
            pwm_config_set_wrap(&pwm, channel->reload_value);
            pwm_init(pwm_gpio_to_slice_num(BEEPER_PIN), &pwm, true);
            const uint16_t level = ((port61 & 3) == 3)
                                     ? (uint16_t)(channel->reload_value / 2)
                                     : 0;
            pwm_set_gpio_level(BEEPER_PIN, level);
        }
    } else {
        // Запись Control Word (порт 0x43)
        const uint8_t access_mode = (data >> 4) & 3;
        i8253_channel_s *channel = &i8253.channels[data >> 6];
        const uint8_t mode = (data >> 1) & 7; // !!! Operating Mode (биты 1-3) !!!

        if (access_mode == PIT_MODE_LATCHCOUNT) {
            channel->latched_value = i8253_get_current_count(channel);
            channel->latch_mode = channel->access_mode;
            channel->byte_toggle = 0;
        } else {
            channel->access_mode = access_mode;
            channel->operating_mode = mode;
            channel->reload_value = 0;
            channel->active = false;
            channel->latch_mode = 0;
            channel->byte_toggle = 0;
            channel->start_timestamp_us = 0;
        }
    }
}
