#ifndef FB_H
#define FB_H
#include <stdint.h>

/* 32bpp framebuffer over the VideoCore property mailbox. */
int  fb_init(uint32_t w, uint32_t h);     /* 1 = ok, 0 = mailbox failed */
uint32_t fb_width(void);
uint32_t fb_height(void);
uint32_t fb_pitch(void);
void *fb_base(void);

#define RGB(r,g,b) (((uint32_t)(r)<<16)|((uint32_t)(g)<<8)|(uint32_t)(b))

void fb_clear(uint32_t color);
void fb_pixel(int x, int y, uint32_t color);
void fb_fill(int x, int y, int w, int h, uint32_t color);

/* --- 5x7 column-major font (welcome/status text) ----------------------- */
/* 5x7 glyph scaled by 'sc'; advances by (FONT_W+1)*sc.  returns x advance */
int  fb_char(int x, int y, char ch, int sc, uint32_t fg);
int  fb_text(int x, int y, const char *s, int sc, uint32_t fg);
int  fb_text_w(const char *s, int sc);    /* pixel width of a string */

/* --- 8x8 IBM font (big clock) ------------------------------------------ */
/* 8x8 glyph scaled by 'sc'; advances by (8+1)*sc.  returns x advance.
 * Renders the glyph as-is (lowercase, digits, ':' all native, no folding). */
int  fb_char8(int x, int y, char ch, int sc, uint32_t fg);
int  fb_text8(int x, int y, const char *s, int sc, uint32_t fg);
int  fb_text8_w(const char *s, int sc);

/* Draw a clock string ("12:34" or "12:34:56") with the 8x8 font, centered
 * horizontally, sized so the glyphs are ~1/4 of the screen height but never
 * wider than the screen. Only repaints when the text changes, so it is cheap
 * to call every loop. 'bg' is used to erase the previous frame's band. */
void fb_clock_big(const char *t, uint32_t fg, uint32_t bg);

/* --- scrolling message ticker (loops, re-enters from the right) -------- */
/* Set text (<=100 chars); call draw periodically at your scroll cadence. */
void fb_marquee_set(const char *s);
void fb_marquee_draw(int y, int sc, int step, uint32_t fg, uint32_t bg);

#endif
