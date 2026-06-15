#ifndef PERIPHERALS_H
#define PERIPHERALS_H

#include <stdint.h>

/*
 * The ONE knob that changes per target board.
 *   BCM2835  (Pi 1 / Zero)        : 0x20000000
 *   BCM2836/7 (Pi 2 / Pi 3)       : 0x3F000000
 *   BCM2711  (Pi 4 / Pi 400)      : 0xFE000000
 * Override from the Makefile with -DPERIPHERAL_BASE=0x.... if you prefer.
 */
#ifndef PERIPHERAL_BASE
#define PERIPHERAL_BASE  0x20000000UL
#endif

#endif /* PERIPHERALS_H */
