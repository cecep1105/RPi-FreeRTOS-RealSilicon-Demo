#ifndef FONT_H
#define FONT_H

#include <stdint.h>

/* 5x7 column-major glyphs, bit0 = top row. Returns a pointer to 5 bytes. */
const uint8_t *glyph(char ch);

/* Stamp a 5-wide glyph into a 32-column framebuffer at column x. */
void fb_put(uint8_t fb[32], int x, const uint8_t g[5]);

#endif /* FONT_H */
