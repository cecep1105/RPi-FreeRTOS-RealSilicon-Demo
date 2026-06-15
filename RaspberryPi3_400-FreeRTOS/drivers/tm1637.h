#ifndef TM1637_H
#define TM1637_H

#include <stdint.h>

/* Pin / timing config (override at build time if wired differently) */
#ifndef TM_CLK
#define TM_CLK    2
#endif
#ifndef TM_DIO
#define TM_DIO    3
#endif
#ifndef TM_US
#define TM_US     5
#endif
#ifndef TM_DELAY
#define TM_DELAY  2000
#endif
#ifndef TM_BRIGHT
#define TM_BRIGHT 7
#endif

void tm1637_init(void);
void tm_number(uint32_t val);            /* right-aligned, blanks leading zeros */
void tm_time(int hh, int mm, int colon);
void tm_dashes(void);

#endif /* TM1637_H */
