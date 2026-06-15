#ifndef FONT8X8_H
#define FONT8X8_H

/* 8x8 monochrome bitmap font, basic Latin U+0000..U+007F.
 * Each glyph is 8 bytes (rows, top->bottom); within a row byte the
 * LEAST significant bit is the LEFTMOST pixel. Public Domain. */
extern const unsigned char font8x8_basic[128][8];

#endif /* FONT8X8_H */
