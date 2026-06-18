#ifndef LA_RENDER_H
#define LA_RENDER_H
#include <stdint.h>
/* Logic-analyzer frame: up to LA_MAXCOL columns, 1 byte/col, bit i = ch i.
 * Wire format (ESP32 -> Pi, each line < 128 bytes):
 *   la begin <ncols> <nch> <rate_hz>
 *   la d <col_off> <hex>          (2 hex chars per column)
 *   la end
 *   la off
 * The Pi reassembles into this struct, then fb_logic_draw() paints it. */
#define LA_MAXCOL 256
typedef struct {
    uint16_t ncols, nch;
    uint32_t rate_hz;
    uint8_t  col[LA_MAXCOL];   /* one byte per column, bit i = channel i */
    uint8_t  ready;            /* set by 'la end' */
} la_frame_t;

void la_reset(la_frame_t *f);                       /* on 'la begin'  */
void la_begin(la_frame_t *f, int ncols,int nch,uint32_t rate);
int  la_chunk(la_frame_t *f, int off, const char *hex);  /* on 'la d'; returns cols decoded */
void la_logic_chrome(const la_frame_t *f);          /* static parts: draw ONCE on entry */
void la_logic_traces(const la_frame_t *f);          /* trace bands: call per frame      */
void la_logic_draw(const la_frame_t *f);            /* chrome + traces (one-shot helper) */
#endif
