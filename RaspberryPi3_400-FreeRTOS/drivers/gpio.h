#ifndef GPIO_H
#define GPIO_H

#include <stdint.h>

/* GPFSEL function-select codes */
#define GPIO_FUNC_INPUT  0u
#define GPIO_FUNC_OUTPUT 1u
#define GPIO_FUNC_ALT0   4u

/* GPPUD pull codes */
#define GPIO_PULL_OFF    0u
#define GPIO_PULL_DOWN   1u
#define GPIO_PULL_UP     2u

void gpio_set_function(uint32_t pin, uint32_t func);
void gpio_set_pull(uint32_t pin, uint32_t pud);

void gpio_set(uint32_t mask);     /* drive given pin-mask high (GPSET0) */
void gpio_clear(uint32_t mask);   /* drive given pin-mask low  (GPCLR0) */
int  gpio_read(uint32_t pin);     /* read one pin level        (GPLEV0) */
uint32_t gpio_level0(void);       /* atomic snapshot of GPIO0..31 levels       */

#endif /* GPIO_H */
