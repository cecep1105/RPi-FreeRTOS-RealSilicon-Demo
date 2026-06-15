#include "gpio.h"
#include "peripherals.h"
#include "timer.h"

#define GPIO_BASE   (PERIPHERAL_BASE + 0x200000UL)
#define GPFSEL0     ((volatile uint32_t *)(GPIO_BASE + 0x00))
#define GPSET0      ((volatile uint32_t *)(GPIO_BASE + 0x1C))
#define GPCLR0      ((volatile uint32_t *)(GPIO_BASE + 0x28))
#define GPLEV0      ((volatile uint32_t *)(GPIO_BASE + 0x34))

void gpio_set_function(uint32_t pin, uint32_t func) {
    volatile uint32_t *reg = GPFSEL0 + (pin / 10);
    uint32_t shift = (pin % 10) * 3;
    uint32_t v = *reg;
    v &= ~(7u << shift);
    v |=  (func << shift);
    *reg = v;
}

#if PERIPHERAL_BASE == 0xFE000000UL
/* ---- BCM2711 (Pi 4 / Pi 400): GPIO_PUP_PDN_CNTRL ----
   Direct read-modify-write, no clock sequence. NOTE: the pull-code
   encoding is the REVERSE of the legacy block below -- here 1=up, 2=down --
   so we translate from the public (legacy-style) GPIO_PULL_* codes. */
#define GPIO_PUP_PDN0 ((volatile uint32_t *)(GPIO_BASE + 0xE4))
void gpio_set_pull(uint32_t pin, uint32_t pud) {
    static const uint32_t to2711[3] = { 0u, 2u, 1u };  /* off, down->2, up->1 */
    uint32_t code = (pud < 3u) ? to2711[pud] : 0u;
    volatile uint32_t *reg = GPIO_PUP_PDN0 + (pin / 16);
    uint32_t shift = (pin % 16) * 2;
    uint32_t v = *reg;
    v &= ~(3u << shift);
    v |=  (code << shift);
    *reg = v;
}
#else
/* ---- BCM2835/6/7 (Pi 1 / 2 / 3): legacy GPPUD + GPPUDCLK sequence ---- */
#define GPPUD       ((volatile uint32_t *)(GPIO_BASE + 0x94))
#define GPPUDCLK0   ((volatile uint32_t *)(GPIO_BASE + 0x98))
void gpio_set_pull(uint32_t pin, uint32_t pud) {
    *GPPUD = pud;             delay_us(5);
    *GPPUDCLK0 = (1u << pin); delay_us(5);
    *GPPUD = 0;               *GPPUDCLK0 = 0;
}
#endif

void gpio_set(uint32_t mask)   { *GPSET0 = mask; }
void gpio_clear(uint32_t mask) { *GPCLR0 = mask; }
int  gpio_read(uint32_t pin)   { return (int)((*GPLEV0 >> pin) & 1u); }
uint32_t gpio_level0(void)     { return *GPLEV0; }
