#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>

/* Crude calibration-free busy-wait (NOP loop). */
void     delay(uint32_t count);

/* 64-bit free-running microsecond counter (BCM system timer). */
uint64_t sys_us(void);

/* Microsecond busy-wait (built on sys_us). */
void     delay_us(uint32_t us);

#endif /* TIMER_H */
