#include "stepper.h"
#include "gpio.h"
#include <stdint.h>

static const uint8_t IN[4] = { 5, 6, 16, 26 };   /* IN1..IN4 */
/* 8-phase half-step sequence; bit i corresponds to IN[i]. */
static const uint8_t SEQ[8] = { 0x1, 0x3, 0x2, 0x6, 0x4, 0xC, 0x8, 0x9 };

void stepper_init(void)
{
    for (int i = 0; i < 4; i++) {
        gpio_set_function(IN[i], GPIO_FUNC_OUTPUT);
        gpio_clear(1u << IN[i]);
    }
}

void stepper_phase(int phase)
{
    uint8_t m = SEQ[phase & 7];
    uint32_t set = 0, clr = 0;
    for (int i = 0; i < 4; i++) {
        if (m & (1u << i)) set |= (1u << IN[i]);
        else               clr |= (1u << IN[i]);
    }
    gpio_clear(clr);
    gpio_set(set);
}

void stepper_release(void)
{
    for (int i = 0; i < 4; i++) gpio_clear(1u << IN[i]);
}
