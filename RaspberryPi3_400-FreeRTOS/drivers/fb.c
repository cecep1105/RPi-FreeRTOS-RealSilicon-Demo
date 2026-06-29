#include "fb.h"
#include "peripherals.h"
#include "font5x7.h"
#include "font8x8.h"
#include "font.h"     /* glyph(): same 5x7 font the MAX7219 uses */

/* ---- VideoCore property mailbox ---------------------------------------- */
#define MBOX_BASE   (PERIPHERAL_BASE + 0xB880)
#define MBOX_READ   (*(volatile uint32_t*)(MBOX_BASE + 0x00))
#define MBOX_STATUS (*(volatile uint32_t*)(MBOX_BASE + 0x18))
#define MBOX_WRITE  (*(volatile uint32_t*)(MBOX_BASE + 0x20))
#define MBOX_FULL   0x80000000u
#define MBOX_EMPTY  0x40000000u

static volatile uint32_t __attribute__((aligned(16))) mbox[40];

static int mbox_call(uint32_t ch)
{
    uint32_t addr = ((uint32_t)(uintptr_t)mbox & ~0xFu) | (ch & 0xF);
    while(MBOX_STATUS & MBOX_FULL) {}
    MBOX_WRITE = addr;
    for(;;){
        while(MBOX_STATUS & MBOX_EMPTY) {}
        if(MBOX_READ == addr) return mbox[1] == 0x80000000u;
    }
}

/* ---- framebuffer state ------------------------------------------------- */
static uint8_t  *g_fb;
static uint32_t  g_pitch, g_w, g_h;

uint32_t fb_width(void){ return g_w; }
uint32_t fb_height(void){ return g_h; }
uint32_t fb_pitch(void){ return g_pitch; }
void    *fb_base(void){ return g_fb; }

int fb_init(uint32_t w, uint32_t h)
{
    int i=0, fb_i, pitch_i;
    mbox[i++]=0;                 /* total size, patched below           */
    mbox[i++]=0;                 /* request                              */
    mbox[i++]=0x00048003; mbox[i++]=8; mbox[i++]=0; mbox[i++]=w; mbox[i++]=h;  /* set phys wh */
    mbox[i++]=0x00048004; mbox[i++]=8; mbox[i++]=0; mbox[i++]=w; mbox[i++]=h;  /* set virt wh */
    mbox[i++]=0x00048009; mbox[i++]=8; mbox[i++]=0; mbox[i++]=0; mbox[i++]=0;  /* set virt off */
    mbox[i++]=0x00048005; mbox[i++]=4; mbox[i++]=0; mbox[i++]=32;              /* set depth   */
    mbox[i++]=0x00048006; mbox[i++]=4; mbox[i++]=0; mbox[i++]=0;               /* pixel order BGR -> RGB() shows correct red; flip to 1 if colours invert */
    mbox[i++]=0x00040001; mbox[i++]=8; mbox[i++]=0; fb_i=i; mbox[i++]=4096; mbox[i++]=0; /* alloc fb */
    mbox[i++]=0x00040008; mbox[i++]=4; mbox[i++]=0; pitch_i=i; mbox[i++]=0;    /* get pitch   */
    mbox[i++]=0;                 /* end tag */
    mbox[0]=i*4;

    if(!mbox_call(8)) return 0;
    g_fb    = (uint8_t*)(uintptr_t)(mbox[fb_i] & 0x3FFFFFFF);  /* bus->ARM phys */
    g_pitch = mbox[pitch_i];
    g_w=w; g_h=h;
    return g_fb != 0;
}

/* ---- primitives -------------------------------------------------------- */
void fb_pixel(int x, int y, uint32_t color)
{
    if((uint32_t)x>=g_w || (uint32_t)y>=g_h) return;
    *(volatile uint32_t*)(g_fb + y*g_pitch + x*4) = color;
}

void fb_fill(int x, int y, int w, int h, uint32_t color)
{
    if(w<=0 || h<=0) return;
    int x1=x, y1=y, x2=x+w, y2=y+h;          /* clip to screen, both edges */
    if(x1<0)x1=0;  if(y1<0)y1=0;
    if(x2>(int)g_w)x2=(int)g_w;  if(y2>(int)g_h)y2=(int)g_h;
    for(int yy=y1; yy<y2; yy++){
        volatile uint32_t *row=(volatile uint32_t*)(g_fb + yy*g_pitch);
        for(int xx=x1; xx<x2; xx++) row[xx]=color;
    }
}

void fb_clear(uint32_t color){ fb_fill(0,0,g_w,g_h,color); }

int fb_char(int x, int y, char ch, int sc, uint32_t fg)
{
    const uint8_t *g = font_glyph(ch);
    for(int c=0;c<FONT_W;c++){
        uint8_t colbits = g[c];
        for(int r=0;r<7;r++){
            if(colbits & (1u<<r)) fb_fill(x+c*sc, y+r*sc, sc, sc, fg);
        }
    }
    return (FONT_W+1)*sc;
}

int fb_text(int x, int y, const char *s, int sc, uint32_t fg)
{
    int x0=x;
    for(int i=0;s[i];i++){ char c=s[i]; if(c>='a'&&c<='z')c-=32; x+=fb_char(x,y,c,sc,fg); }
    return x-x0;
}

int fb_text_w(const char *s, int sc)
{
    int n=0; while(s[n]) n++;
    return n*(FONT_W+1)*sc;
}

/* ======================================================================= *
 *  8x8 IBM font helpers (kept for general text; clock no longer uses them) *
 * ======================================================================= */
int fb_char8(int x, int y, char ch, int sc, uint32_t fg)
{
    const unsigned char *g = font8x8_basic[(unsigned char)ch & 0x7F];
    for(int row=0; row<8; row++){ unsigned char b=g[row];
        for(int col=0; col<8; col++) if(b&(1u<<col)) fb_fill(x+col*sc,y+row*sc,sc,sc,fg); }
    return (8+1)*sc;
}
int fb_text8(int x, int y, const char *s, int sc, uint32_t fg)
{ int x0=x; for(int i=0;s[i];i++) x+=fb_char8(x,y,s[i],sc,fg); return x-x0; }
int fb_text8_w(const char *s, int sc){ int n=0; while(s[n])n++; return n*(8+1)*sc; }

/* ======================================================================= *
 *  Seven-segment "digital" clock — segments drawn as filled bars.         *
 *  FLICKER-FREE: repaints only the digits that changed, and paints each    *
 *  segment straight to its on/off colour (no blank-then-redraw pass).      *
 *  Bold by construction; SEG_T_DIV controls weight (smaller = bolder).     *
 * ======================================================================= */
#ifndef SEG_T_DIV
#define SEG_T_DIV 8      /* bar thickness = digit_height / SEG_T_DIV (smaller = bolder) */
#endif

/* ======================================================================= *
 *  Dot-matrix "LED panel" clock — same 5x7 font as the MAX7219, rendered  *
 *  as round dots. Drawn at native dot pitch (no bitmap scaling), so it is  *
 *  smooth. Flicker-free: repaints only the character cells that changed,   *
 *  each dot written opaquely (lit = fg, unlit = bg).                       *
 * ======================================================================= */
#ifndef DOT_PCT
#define DOT_PCT 80       /* dot diameter as % of cell pitch (bigger = denser) */
#endif
#ifndef DOT_OFF_DIV
#define DOT_OFF_DIV 6    /* off-LED brightness = on-colour / DOT_OFF_DIV (bigger = dimmer) */
#endif

/* Dim an on-colour to the unlit-LED colour (keeps the same hue as fg). */
static uint32_t dim_color(uint32_t c)
{
    int r=(c>>16)&0xFF, g=(c>>8)&0xFF, b=c&0xFF;
    return RGB(r/DOT_OFF_DIV, g/DOT_OFF_DIV, b/DOT_OFF_DIV);
}

/* Filled circle, integer-only (no libm / no FPU needed). */
static void fb_disc(int cx,int cy,int r,uint32_t col)
{
    if(r<1){ fb_fill(cx,cy,1,1,col); return; }
    int r2=r*r;
    for(int dy=-r; dy<=r; dy++){
        int hw=0; while((hw+1)*(hw+1)+dy*dy<=r2) hw++;
        fb_fill(cx-hw, cy+dy, 2*hw+1, 1, col);
    }
}

/* Paint one 5x7 character as a dot grid at cell origin (x,y), pitch 'cell'. */
static void mtx_char(int x,int y,char ch,int cell,int dotr,uint32_t fg,uint32_t off)
{
    const uint8_t *g = glyph(ch);              /* 5 bytes, column-major, bit0=top */
    for(int col=0; col<5; col++)
        for(int row=0; row<7; row++){
            int lit = (g[col]>>row)&1;
            int cx = x + col*cell + cell/2;
            int cy = y + row*cell + cell/2;
            fb_disc(cx, cy, dotr, lit?fg:off);
        }
}

/* Dot-matrix clock, centered, ~1/4 screen height (shrinks only to fit width).
 * Repaints only changed cells. Pass a space in a colon slot ("12 34") to blink
 * the colon without disturbing layout. Call with fg = RGB(255,0,0) for red. */
/* Invalidate the incremental-redraw cache so the next fb_clock_big() fully
 * repaints every dot. Call right after clearing the screen (e.g. when leaving
 * the QR view) so digits whose value did not change are not left blank. */
static int g_clock_force = 0;
void fb_clock_reset(void){ g_clock_force = 1; }

void fb_clock_big(const char *s, uint32_t fg, uint32_t bg)
{
    static char last[16]={0};
    static int  pCell=-1, px0=0, py0=0, pH=0, pW=0;
    if(g_clock_force){ pCell=-1; last[0]=0; g_clock_force=0; }  /* force full repaint */

    int n=0; while(s[n]) n++;
    const int CHW = 6;                          /* 5 cols + 1 col gap, in cells */

    int cell = (int)fb_height()/4/7;            /* dot pitch for ~1/4 height (7 rows) */
    if(cell<2) cell=2;
    for(;;){                                     /* shrink to fit 95% of the width */
        int cells = n*CHW - 1;                   /* drop trailing gap column */
        if(cells*cell <= (int)fb_width()*95/100 || cell<=2) break;
        cell--;
    }
    int dotr = (cell*DOT_PCT)/200;  if(dotr<1) dotr=1;
    int H = 7*cell;
    int W = (n*CHW - 1)*cell;
    int x0 = ((int)fb_width()-W)/2;
    int y0 = H/2;            

    uint32_t off = dim_color(fg);                /* unlit-LED colour (dim fg) */

    int relayout = (cell!=pCell || x0!=px0 || y0!=py0);
    if(relayout){
        if(pCell>0) fb_fill(px0, py0, pW, pH, bg);   /* erase old clock once */
        fb_fill(x0, y0, W, H, bg);                   /* clear new area once  */
        /* paint the full panel of OFF dots once (every LED position visible) */
        int gcols = n*CHW - 1;
        for(int gc=0; gc<gcols; gc++)
            for(int row=0; row<7; row++)
                fb_disc(x0+gc*cell+cell/2, y0+row*cell+cell/2, dotr, off);
        last[0]=0;                                    /* force all chars to repaint */
        pCell=cell; px0=x0; py0=y0; pH=H; pW=W;
    }

    int x=x0;
    for(int i=0;i<n;i++){
        if(relayout || s[i]!=last[i]) mtx_char(x, y0, s[i], cell, dotr, fg, off);
        x += CHW*cell;
    }
    int i=0; for(; s[i] && i<15; i++) last[i]=s[i]; last[i]=0;
}

/* ======================================================================= *
 *  Scrolling message ticker — 5x7 font, loops continuously.               *
 *  Text up to MSG_MAX chars (set from the serial 'msg' command). Scrolls   *
 *  right -> left; when the last character exits the left, the message      *
 *  re-enters from the right edge. Flicker-free: every band pixel is        *
 *  written exactly once per draw (opaque cells + bg margins, no clear).    *
 * ======================================================================= */
#define MSG_MAX 500
static char mq_text[MSG_MAX+1] = "";
static int  mq_len  = 0;
static int  mq_head = -1;                 /* scroll offset (px); -1 = (re)start */

/* Set the ticker text (truncated to 100 chars) and restart from the right. */
void fb_marquee_set(const char *s)
{
    int i=0; for(; s[i] && i<MSG_MAX; i++) mq_text[i]=s[i];
    mq_text[i]=0; mq_len=i; mq_head=-1;
}

/* Draw + advance the ticker on a band at y, 5x7 font scaled by sc, moving
 * 'step' px per call. Call this at your scroll cadence. */
void fb_marquee_draw(int y, int sc, int step, uint32_t fg, uint32_t bg)
{
    int W      = (int)fb_width();
    int cellW  = (FONT_W+1)*sc;            /* 5 glyph cols + 1 gap col */
    int band_h = 7*sc;
    int textW  = mq_len*cellW;
    int span   = W + textW;                /* one full pass: enter right .. exit left */
    if(mq_len==0){ fb_fill(0,y,W,band_h,bg); return; }

    if(mq_head < 0) mq_head = 0;           /* 0 => first char just off the right edge */
    else { mq_head += step; if(mq_head >= span) mq_head -= span; }

    int x0   = W - mq_head;                /* left edge of the message */
    int xend = x0 + textW;                 /* right edge of the message */

    if(x0   > 0) fb_fill(0,    y, x0,     band_h, bg);   /* left margin  */
    if(xend < W) fb_fill(xend, y, W-xend, band_h, bg);   /* right margin */

    for(int i=0;i<mq_len;i++){
        int cx = x0 + i*cellW;
        if(cx >= W) break;                 /* this and all later chars off-right */
        if(cx + cellW <= 0) continue;      /* off-left */
        const uint8_t *g = font_glyph(mq_text[i]);
        for(int col=0; col<FONT_W; col++)
            for(int row=0; row<7; row++)
                fb_fill(cx+col*sc, y+row*sc, sc, sc, ((g[col]>>row)&1)?fg:bg);
        fb_fill(cx+FONT_W*sc, y, sc, band_h, bg);        /* inter-char gap column */
    }
}
