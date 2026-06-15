#include "pwm.h"
#include "peripherals.h"
#include "gpio.h"
#include "timer.h"
#include <stdint.h>

#define PWM_BASE  (PERIPHERAL_BASE + 0x20C000UL)
#define PWM_CTL   ((volatile uint32_t *)(PWM_BASE + 0x00))
#define PWM_RNG1  ((volatile uint32_t *)(PWM_BASE + 0x10))
#define PWM_DAT1  ((volatile uint32_t *)(PWM_BASE + 0x14))

#define CM_BASE   (PERIPHERAL_BASE + 0x101000UL)
#define CM_PWMCTL ((volatile uint32_t *)(CM_BASE + 0xA0))
#define CM_PWMDIV ((volatile uint32_t *)(CM_BASE + 0xA4))
#define CM_PASSWD 0x5A000000u
#define CM_ENAB   (1u << 4)
#define CM_BUSY   (1u << 7)
#define CM_SRC_OSC 1u
#define PWM_PWEN1 (1u << 0)
#define PWM_MSEN1 (1u << 7)

/* Clock-manager oscillator feeding the PWM (board dependent). */
#if (PERIPHERAL_BASE == 0xFE000000UL)
#define PWM_OSC_HZ 54000000u     /* BCM2711 */
#else
#define PWM_OSC_HZ 19200000u     /* BCM2835/6/7 */
#endif
#define PWM_DIVI (PWM_OSC_HZ / 1000000u)                         /* -> 1 MHz */
#define PWM_DIVF (((PWM_OSC_HZ % 1000000u) * 4096u) / 1000000u)
#define SERVO_RANGE 20000u       /* 20 ms @ 1 MHz -> 50 Hz; 1 count = 1 us */

static int g_us = 1500;

void servo_init(void)
{
    gpio_set_function(12, GPIO_FUNC_ALT0);     /* GPIO12 = PWM0 */
    *PWM_CTL = 0;
    delay_us(10);
    /* stop the PWM clock, then reprogram its divider from the oscillator */
    *CM_PWMCTL = CM_PASSWD | (*CM_PWMCTL & ~CM_ENAB);
    { uint32_t g = 100000u; while ((*CM_PWMCTL & CM_BUSY) && --g) delay_us(1); }
    *CM_PWMDIV = CM_PASSWD | (PWM_DIVI << 12) | PWM_DIVF;
    *CM_PWMCTL = CM_PASSWD | CM_SRC_OSC | CM_ENAB;
    { uint32_t g = 100000u; while (!(*CM_PWMCTL & CM_BUSY) && --g) delay_us(1); }
    delay_us(10);
    *PWM_RNG1 = SERVO_RANGE;
    *PWM_DAT1 = (uint32_t)g_us;
    *PWM_CTL  = PWM_MSEN1 | PWM_PWEN1;          /* mark-space, channel 1 on */
}

void servo_set_us(int us)
{
    if (us < 500)  us = 500;
    if (us > 2500) us = 2500;
    g_us = us;
    *PWM_DAT1 = (uint32_t)us;
}

void servo_set_angle(int deg)
{
    if (deg < 0)   deg = 0;
    if (deg > 180) deg = 180;
    servo_set_us(1000 + deg * 1000 / 180);      /* 0deg=1.0ms .. 180deg=2.0ms */
}

int servo_get_angle(void) { return (g_us - 1000) * 180 / 1000; }
