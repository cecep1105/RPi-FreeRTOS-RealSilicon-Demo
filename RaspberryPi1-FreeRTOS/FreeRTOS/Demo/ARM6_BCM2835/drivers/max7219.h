#ifndef MAX7219_H
#define MAX7219_H

#include <stdint.h>

/* ---- 4-in-1 module orientation knobs (override at build time if wrong) ---- */
#ifndef MAT_REVERSE_CHIPS
#define MAT_REVERSE_CHIPS 0   /* 1 if the 4 modules appear in reversed order   */
#endif
#ifndef MAT_MIRROR_X
#define MAT_MIRROR_X      0   /* 1 if each module is mirrored horizontally     */
#endif
#ifndef MAT_FLIP_Y
#define MAT_FLIP_Y        0   /* 1 if rows are upside down                     */
#endif
#ifndef MAT_INTENSITY
#define MAT_INTENSITY     2   /* 0..15                                         */
#endif

void max_init(void);
void max_all(uint8_t reg, uint8_t val);    /* same command to all 4 chips */
void max_render(const uint8_t fb[32]);     /* 32 cols, bit r = row r (0=top) */

#endif /* MAX7219_H */
