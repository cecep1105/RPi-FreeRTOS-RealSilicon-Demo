#ifndef DISPLAY_H
#define DISPLAY_H

/* High-level MAX7219 matrix views, built on font + max7219. */

void matrix_clock(int hh, int mm, int colon);

/* Scroll a string across the matrix. Under FreeRTOS the inter-frame wait is a
   vTaskDelay, so other tasks (UART, sweep, TM1637) keep running during the
   scroll -- no yield callback needed. */
void matrix_scroll(const char *s);

#endif /* DISPLAY_H */
