#include "display.h"
#include "max7219.h"
#include "font.h"

#include "FreeRTOS.h"
#include "task.h"

/* per-column shift time while scrolling; lower = faster (tune to taste) */
#ifndef SCROLL_FRAME_MS
#define SCROLL_FRAME_MS 25
#endif

void matrix_clock(int hh, int mm, int colon) {
    uint8_t fb[32]; for (int i = 0; i < 32; i++) fb[i] = 0;
    fb_put(fb, 2,  glyph((char)('0' + (hh / 10))));
    fb_put(fb, 8,  glyph((char)('0' + (hh % 10))));
    if (colon) fb_put(fb, 14, glyph(':'));
    fb_put(fb, 17, glyph((char)('0' + (mm / 10))));
    fb_put(fb, 23, glyph((char)('0' + (mm % 10))));
    max_render(fb);
}

void matrix_scroll(const char *s) {
    uint8_t msg[200]; int w = 0;
    for (int i = 0; i < 32; i++) msg[w++] = 0;                 /* lead-in */
    for (const char *p = s; *p && w < 190; p++) {
        const uint8_t *g = glyph(*p);
        for (int i = 0; i < 5; i++) msg[w++] = g[i];
        msg[w++] = 0;                                          /* 1px gap */
    }
    for (int i = 0; i < 32; i++) msg[w++] = 0;                 /* lead-out */
    for (int off = 0; off + 32 <= w; off++) {
        uint8_t fb[32];
        for (int c = 0; c < 32; c++) fb[c] = msg[off + c];
        max_render(fb);
        vTaskDelay(pdMS_TO_TICKS(SCROLL_FRAME_MS));            /* was: delay()+yield */
    }
}
