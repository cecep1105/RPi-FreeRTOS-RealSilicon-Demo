#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>

/* Crude calibration-free busy-wait (NOP loop). */
void     delay(uint32_t count);

/* 64-bit free-running microsecond counter (BCM system timer). */
uint64_t sys_us(void);

#endif /* TIMER_H */

/* Accurate microsecond busy-wait (system-timer based; clock-independent). */
void delay_us(uint32_t us);
