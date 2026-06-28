#include "timer.h"
#include "peripherals.h"

#define STIMER_BASE (PERIPHERAL_BASE + 0x3000UL)
#define STIMER_CLO  ((volatile uint32_t *)(STIMER_BASE + 0x04))
#define STIMER_CHI  ((volatile uint32_t *)(STIMER_BASE + 0x08))

void delay(volatile uint32_t count) {
    while (count--) { __asm__ volatile("nop"); }
}

uint64_t sys_us(void) {
    uint32_t hi, lo, hi2;
    do { hi = *STIMER_CHI; lo = *STIMER_CLO; hi2 = *STIMER_CHI; } while (hi != hi2);
    return ((uint64_t)hi << 32) | lo;
}

/* Microsecond busy-wait built on the 1 MHz system timer. Used by the USB
 * Ethernet driver (usb_dwc2.c) and the GENET shim. Pre-scheduler safe. */
void delay_us(uint32_t us) {
    uint64_t start = sys_us();
    uint32_t guard = us * 200u + 100000u;   /* bail if the timer never advances */
    while ((sys_us() - start) < (uint64_t)us) {
        if (guard-- == 0u) break;
    }
}
