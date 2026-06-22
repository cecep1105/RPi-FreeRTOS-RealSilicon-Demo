/* sc_render.c — oscilloscope renderer for the Pi framebuffer. Uses only the
 * fb_* API (fb_clear/fb_fill/fb_text/fb_width/fb_height/RGB). Mirrors the
 * logic renderer: chrome drawn once, waveforms repainted per-column with no
 * full-area blanking so the trace never flickers. */
#include "sc_render.h"

extern uint32_t fb_width(void);
extern uint32_t fb_height(void);
extern void     fb_clear(uint32_t c);
extern void     fb_fill(int x,int y,int w,int h,uint32_t c);
extern int      fb_text(int x,int y,const char*s,int sc,uint32_t fg);

#ifndef RGB
#define RGB(r,g,b) (((uint32_t)(r)<<16)|((uint32_t)(g)<<8)|(uint32_t)(b))
#endif

#define SC_BG   RGB(8,10,14)
#define SC_GRID RGB(26,30,38)
#define SC_AXIS RGB(44,50,62)
#define SC_INK  RGB(230,235,245)
static const uint32_t CH_COL[2] = { RGB(240,220,60), RGB(70,210,235) }; /* A=amber B=cyan */

#define SC_DIVX 10           /* horizontal divisions (time) */
#define SC_DIVY 8            /* vertical divisions (volts)  */

static int hexval(char c){
    if(c>='0'&&c<='9') return c-'0';
    if(c>='a'&&c<='f') return c-'a'+10;
    if(c>='A'&&c<='F') return c-'A'+10;
    return -1;
}

/* ---- wire decode ----------------------------------------------------- */
void sc_reset(sc_frame_t *f){ f->ncols=0; f->nch=1; f->rate_hz=0; f->ready=0; }
void sc_begin(sc_frame_t *f, int ncols,int nch,uint32_t rate){
    if(ncols<1) ncols=1;
    if(ncols>SC_MAXCOL) ncols=SC_MAXCOL;
    f->ncols=(uint16_t)ncols; f->nch=(uint16_t)((nch<1)?1:(nch>2?2:nch));
    f->rate_hz=rate; f->ready=0;
}
int sc_chunk(sc_frame_t *f, int chan, int off, const char *hex){
    if(chan<0||chan>1) return 0;
    int n=0;
    while(hex[0] && hex[1] && off+n < f->ncols){
        int hi=hexval(hex[0]), lo=hexval(hex[1]);
        if(hi<0||lo<0) break;
        f->ch[chan][off+n]=(uint8_t)((hi<<4)|lo);
        hex+=2; n++;
    }
    return n;
}

/* ---- geometry -------------------------------------------------------- */
static void sc_geom(int *plotx,int *ploty,int *plotw,int *ploth,int *cpw,const sc_frame_t *f){
    int W=(int)fb_width(), H=(int)fb_height();
    int L=44, R=14, TOP=56, BOT=26;
    *plotx=L; *ploty=TOP; *plotw=W-L-R; *ploth=H-TOP-BOT;
    int nc=(f->ncols<1)?1:f->ncols;
    *cpw=(*plotw)/nc; if(*cpw<1)*cpw=1;
}
/* sample (0..255) -> y within the plot (255 at top) */
static int sc_y(int ploty,int ploth,int s){ return ploty + ((255-s)*ploth)/255; }

static int cap_u(char *cap,int c,unsigned v){
    char t[12]; int n=0; if(v==0)t[n++]='0'; while(v){t[n++]=(char)('0'+v%10); v/=10;}
    while(n>0) cap[c++]=t[--n];
    return c;
}

/* append "<v.v><unit>/div" for a window of `winsamp` samples at `rate_hz`,
 * split into `ndiv` divisions. integer math in picoseconds, one decimal. */
static int cap_timediv(char *cap,int c,uint32_t rate_hz,int winsamp,int ndiv){
    if(rate_hz==0 || winsamp<=0 || ndiv<=0){ cap[c++]='-'; return c; }
    unsigned long long ps = (unsigned long long)winsamp * 1000000000000ULL
                            / ((unsigned long long)rate_hz * (unsigned)ndiv);
    const char *u; unsigned long long sc;
    if(ps>=1000000000ULL){ u="ms"; sc=1000000000ULL; }
    else if(ps>=1000000ULL){ u="us"; sc=1000000ULL; }
    else { u="ns"; sc=1000ULL; }
    unsigned long long v10=(ps*10ULL + sc/2)/sc;       /* value x10, rounded */
    c=cap_u(cap,c,(unsigned)(v10/10));
    cap[c++]='.'; cap[c++]=(char)('0'+(unsigned)(v10%10));
    cap[c++]=u[0]; cap[c++]=u[1];
    cap[c++]='/'; cap[c++]='d'; cap[c++]='i'; cap[c++]='v';
    return c;
}

/* ---- chrome (draw once) ---------------------------------------------- */
void sc_scope_chrome(const sc_frame_t *f){
    int plotx,ploty,plotw,ploth,cpw; sc_geom(&plotx,&ploty,&plotw,&ploth,&cpw,f);
    int H=(int)fb_height();
    fb_clear(SC_BG);
    fb_text(16,14,"OSCILLOSCOPE  BS05U",2,SC_INK);

    char cap[56]; int c=0;
    const char *s="CH"; cap[c++]=s[0]; cap[c++]=s[1];
    cap[c++]='A'; if(f->nch>1){ cap[c++]='+'; cap[c++]='B'; }
    cap[c++]=' '; cap[c++]=' ';
    c=cap_u(cap,c,f->ncols); cap[c++]='S'; cap[c++]=' '; cap[c++]=' ';
    if(f->rate_hz>=1000000){ c=cap_u(cap,c,f->rate_hz/1000000); cap[c++]='M'; }
    else                   { c=cap_u(cap,c,f->rate_hz/1000);    cap[c++]='K'; }
    cap[c++]='S'; cap[c++]='/'; cap[c++]='s';
    cap[c++]=' '; cap[c++]=' ';
    c=cap_timediv(cap,c,f->rate_hz,(int)f->ncols,SC_DIVX);   /* scope: 1 samp/col */
    cap[c]=0;
    fb_text(16,44,cap,2,RGB(150,160,180));

    /* legend */
    fb_text(plotx+plotw-150,16,"CHA",2,CH_COL[0]);
    if(f->nch>1) fb_text(plotx+plotw-72,16,"CHB",2,CH_COL[1]);

    /* grid: vertical + horizontal division lines, brighter centre axes */
    for(int i=0;i<=SC_DIVX;i++){
        int x=plotx+(plotw*i)/SC_DIVX;
        fb_fill(x,ploty,1,ploth,(i==SC_DIVX/2)?SC_AXIS:SC_GRID);
    }
    for(int i=0;i<=SC_DIVY;i++){
        int y=ploty+(ploth*i)/SC_DIVY;
        fb_fill(plotx,y,plotw,1,(i==SC_DIVY/2)?SC_AXIS:SC_GRID);
    }
    fb_text(16,H-18,"scope off = back to clock",1,RGB(90,98,112));
}

/* Redraw the grid pixels that fall inside one column strip [px,px+cpw). */
static void sc_grid_strip(int px,int cpw,int plotx,int ploty,int plotw,int ploth){
    for(int i=0;i<=SC_DIVY;i++){
        int y=ploty+(ploth*i)/SC_DIVY;
        fb_fill(px,y,cpw,1,(i==SC_DIVY/2)?SC_AXIS:SC_GRID);
    }
    for(int i=0;i<=SC_DIVX;i++){
        int gx=plotx+(plotw*i)/SC_DIVX;
        if(gx>=px && gx<px+cpw) fb_fill(gx,ploty,1,ploth,(i==SC_DIVX/2)?SC_AXIS:SC_GRID);
    }
}

/* ---- waveforms (per frame) ------------------------------------------- */
void sc_scope_traces(const sc_frame_t *f){
    int plotx,ploty,plotw,ploth,cpw; sc_geom(&plotx,&ploty,&plotw,&ploth,&cpw,f);
    const int th=2;                       /* trace thickness */
    int prev_y[2];
    for(int ch=0; ch<f->nch; ch++) prev_y[ch]=sc_y(ploty,ploth,f->ch[ch][0]);

    for(int x=0; x<(int)f->ncols; x++){
        int px=plotx+x*cpw;
        /* repaint the whole strip: bg + grid first (no black flash), then ink */
        fb_fill(px,ploty,cpw,ploth,SC_BG);
        sc_grid_strip(px,cpw,plotx,ploty,plotw,ploth);
        for(int ch=0; ch<f->nch; ch++){
            int y=sc_y(ploty,ploth,f->ch[ch][x]);
            int a=(y<prev_y[ch])?y:prev_y[ch];     /* connect prev sample -> this */
            int b=(y<prev_y[ch])?prev_y[ch]:y;
            fb_fill(px,a,cpw,(b-a)+th,CH_COL[ch]);
            prev_y[ch]=y;
        }
    }
}

void sc_scope_draw(const sc_frame_t *f){ sc_scope_chrome(f); sc_scope_traces(f); }