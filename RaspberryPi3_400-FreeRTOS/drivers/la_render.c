/* la_render.c — Raspberry Pi (bare-metal + FreeRTOS) logic-analyzer view.
 *
 * Depends ONLY on the existing framebuffer API (fb.h): fb_clear, fb_fill,
 * fb_text, fb_width, fb_height, RGB(). Drop-in alongside qr_paint*; called
 * from vHdmi when g_la_req is raised, and reverted by 'la off' exactly like
 * the QR full-screen view.
 */
#include "la_render.h"
#include "fb.h"

/* --- tiny local helpers (no libc dependency) --------------------------- */
static int hexnyb(char c){
    if(c>='0'&&c<='9') return c-'0';
    if(c>='a'&&c<='f') return c-'a'+10;
    if(c>='A'&&c<='F') return c-'A'+10;
    return -1;
}

void la_reset(la_frame_t *f){
    f->ncols=0; f->nch=8; f->rate_hz=0; f->ready=0;
    for(int i=0;i<LA_MAXCOL;i++) f->col[i]=0;
}

void la_begin(la_frame_t *f,int ncols,int nch,uint32_t rate){
    la_reset(f);
    if(ncols<0) ncols=0;
    if(ncols>LA_MAXCOL) ncols=LA_MAXCOL;
    if(nch<1) nch=1;
    if(nch>8) nch=8;
    f->ncols=(uint16_t)ncols; f->nch=(uint16_t)nch; f->rate_hz=rate;
}

/* decode one 'la d <off> <hex>' chunk into the column buffer */
int la_chunk(la_frame_t *f,int off,const char *hex){
    int n=0;
    while(hex[0] && hex[1]){
        int hi=hexnyb(hex[0]), lo=hexnyb(hex[1]);
        if(hi<0||lo<0) break;
        int idx=off+n;
        if(idx>=0 && idx<LA_MAXCOL && idx<f->ncols) f->col[idx]=(uint8_t)((hi<<4)|lo);
        n++; hex+=2;
    }
    return n;
}

/* 8-lane bright palette (one colour per logic channel) */
static const uint32_t LANE_COL[8] = {
    0x00E6194B, 0x003CB44B, 0x00FFE119, 0x004363D8,
    0x00F58231, 0x00911EB4, 0x0046F0F0, 0x00F032E6
};

#define LA_BG   RGB(8,10,14)
#define LA_GRID RGB(28,32,40)
#define LA_INK  RGB(230,235,245)
#define LA_DIVX 10   /* notional time divisions across the plot (for time/div) */
#define LA_OVS  2    /* ESP packs BS_NSAMPLES(480) into BS_NCOLS(240): 2 samp/col */

/* shared lane geometry so chrome and traces agree exactly */
static void la_geom(const la_frame_t *f,int *TOP,int *plotx,int *plotw,
                    int *lanes,int *laneh,int *amp,int *cpw){
    const int W=(int)fb_width(), H=(int)fb_height();
    const int top=78, BOT=24, LBL=64;
    *TOP=top; *plotx=LBL; *plotw=W-LBL-16;
    *lanes=(f->nch<1)?1:(f->nch>8?8:f->nch);
    *laneh=(H-top-BOT)/(*lanes);
    *amp=((*laneh)*6)/10;
    int colpx=(f->ncols>0)?(*plotw/(int)f->ncols):*plotw;
    *cpw=(colpx<1)?1:colpx;
}

static void la_lane_levels(int TOP,int laneh,int amp,int ln,int *y_lo,int *y_hi){
    int lane_top=TOP+ln*laneh;
    *y_lo=lane_top+laneh-(laneh-amp)/2-2;
    *y_hi=*y_lo-amp;
}

/* append unsigned decimal to cap[] at index c; returns new index */
static int cap_u(char *cap, int c, unsigned v){
    char d[10]; int nd=0;
    if(v==0) d[nd++]='0';
    while(v){ d[nd++]=(char)('0'+v%10); v/=10; }
    while(nd) cap[c++]=d[--nd];
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

/* Draw the static parts ONCE when entering logic mode (header, caption,
 * channel labels, lane separators, footer). */
void la_logic_chrome(const la_frame_t *f){
    const int H=(int)fb_height();
    int TOP,plotx,plotw,lanes,laneh,amp,cpw;
    la_geom(f,&TOP,&plotx,&plotw,&lanes,&laneh,&amp,&cpw);

    fb_clear(LA_BG);
    fb_text(16,14,"LOGIC ANALYZER  BS05U",2,LA_INK);

    /* caption: "<nch>ch <ncols>col <rate>" with MHz/kHz auto units */
    char cap[64]; int c=0;
    cap[c++]=(char)('0'+(f->nch%10));
    cap[c++]='c'; cap[c++]='h'; cap[c++]=' ';
    c = cap_u(cap, c, f->ncols);
    cap[c++]='c'; cap[c++]='o'; cap[c++]='l'; cap[c++]=' ';
    if(f->rate_hz>=1000000){
        c = cap_u(cap, c, f->rate_hz/1000000);
        cap[c++]='M';
    } else {
        c = cap_u(cap, c, f->rate_hz/1000);
        cap[c++]='k';
    }
    cap[c++]='H'; cap[c++]='z';
    cap[c++]=' '; cap[c++]=' ';
    c = cap_timediv(cap, c, f->rate_hz, (int)f->ncols * LA_OVS, LA_DIVX);
    cap[c]=0;
    fb_text(16,44,cap,2,RGB(150,160,180));

    for(int ln=0; ln<lanes; ln++){
        int y_lo,y_hi; la_lane_levels(TOP,laneh,amp,ln,&y_lo,&y_hi);
        fb_fill(plotx,TOP+ln*laneh+laneh-1,plotw,1,LA_GRID);   /* separator */
        /* L5 is hardware-shared with the BS05 internal clock generator */
        char lab[4]={'L',(char)('0'+ln),0,0};
        if(ln==5){ lab[0]='C'; lab[1]='L'; lab[2]='K'; }
        fb_text(14,(y_lo+y_hi)/2-7,lab,2,LANE_COL[ln]);
    }
    fb_text(16,H-18,"la off = back to clock",1,RGB(90,98,112));

    /* faint vertical time divisions across the full lane stack (LA_DIVX cells).
     * The per-frame trace repaint restores these inside each band; the chrome
     * pass covers the separator gaps so the lines look continuous. */
    {
        int gtop = TOP, gbot = TOP + lanes*laneh;
        for(int i=1;i<LA_DIVX;i++){
            int gx = plotx + (plotw*i)/LA_DIVX;
            fb_fill(gx, gtop, 1, gbot-gtop, LA_GRID);
        }
    }
}

/* Repaint ONLY the trace bands (call once per frame in continuous mode).
 * Each column strip is painted in a single top->bottom sweep (bg, ink, bg) so
 * no part of the band is ever cleared to black first -> no flicker/tearing. */
void la_logic_traces(const la_frame_t *f){
    int TOP,plotx,plotw,lanes,laneh,amp,cpw;
    la_geom(f,&TOP,&plotx,&plotw,&lanes,&laneh,&amp,&cpw);
    const int th=2;
    for(int ln=0; ln<lanes; ln++){
        int y_lo,y_hi; la_lane_levels(TOP,laneh,amp,ln,&y_lo,&y_hi);
        int band_top=y_hi-3, band_bot=y_lo+th+3;     /* band vertical extent */
        int prev_bit=-1, prev_y=y_lo;
        for(int x=0; x<(int)f->ncols; x++){
            int bit=(f->col[x]>>ln)&1;
            int y=bit?y_hi:y_lo, px=plotx+x*cpw;
            /* lane-colour ("ink") span for this column = the level run, grown
               to include the rising/falling edge wherever the bit changes */
            int ink_top=y, ink_bot=y+th;
            if(prev_bit>=0 && bit!=prev_bit){
                int ya=(y<prev_y)?y:prev_y, yb=(y<prev_y)?prev_y:y;
                ink_top=ya; ink_bot=yb+th;
            }
            fb_fill(px, band_top, cpw, ink_top-band_top, LA_BG);      /* bg above */
            fb_fill(px, ink_top,  cpw, ink_bot-ink_top,  LANE_COL[ln]);/* trace   */
            fb_fill(px, ink_bot,  cpw, band_bot-ink_bot, LA_BG);      /* bg below */
            /* restore the vertical time-division line under/around the trace so
             * the per-frame repaint doesn't erase it (trace ink stays on top) */
            for(int gi=1; gi<LA_DIVX; gi++){
                int gx=plotx+(plotw*gi)/LA_DIVX;
                if(gx>=px && gx<px+cpw){
                    if(ink_top>band_top) fb_fill(gx, band_top, 1, ink_top-band_top, LA_GRID);
                    if(band_bot>ink_bot) fb_fill(gx, ink_bot,  1, band_bot-ink_bot, LA_GRID);
                }
            }
            prev_bit=bit; prev_y=y;
        }
    }
}

/* convenience: full draw (chrome + traces) */
void la_logic_draw(const la_frame_t *f){ la_logic_chrome(f); la_logic_traces(f); }