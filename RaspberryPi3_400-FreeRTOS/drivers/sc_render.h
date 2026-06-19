#ifndef SC_RENDER_H
#define SC_RENDER_H
#include <stdint.h>
/* Oscilloscope frame: up to 2 analog channels (A,B), 1 byte/sample (0-255,
 * raw 8-bit ADC). Wire format (ESP32 -> Pi, each line < 128 bytes):
 *   sc begin <ncols> <nch> <rate_hz>
 *   sc d <ch> <off> <hex>     (2 hex chars per sample; ch 0=A, 1=B)
 *   sc end
 *   sc off                    (revert to clock)
 * The Pi reassembles into this struct, then sc_scope_draw() paints it. */
#define SC_MAXCOL 480
typedef struct {
    uint16_t ncols, nch;          /* nch = 1 (A) or 2 (A+B) */
    uint32_t rate_hz;
    uint8_t  ch[2][SC_MAXCOL];    /* ch[0]=A, ch[1]=B, 8-bit samples */
    uint8_t  ready;               /* set by 'sc end' */
} sc_frame_t;

void sc_reset(sc_frame_t *f);                                  /* on 'sc begin' */
void sc_begin(sc_frame_t *f, int ncols, int nch, uint32_t rate);
int  sc_chunk(sc_frame_t *f, int chan, int off, const char *hex); /* on 'sc d' */
void sc_scope_chrome(const sc_frame_t *f);   /* static parts: draw ONCE on entry */
void sc_scope_traces(const sc_frame_t *f);   /* waveforms: call per frame        */
void sc_scope_draw(const sc_frame_t *f);     /* chrome + traces (one-shot)        */
#endif
