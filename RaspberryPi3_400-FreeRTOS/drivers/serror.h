#ifndef SERROR_H
#define SERROR_H
#include <stdint.h>
/* Updated by the SError handler (serror_report). */
extern volatile uint32_t g_serror_count;
extern volatile uint64_t g_serror_esr;
extern volatile uint64_t g_serror_far;
extern volatile uint64_t g_serror_elr;
#endif