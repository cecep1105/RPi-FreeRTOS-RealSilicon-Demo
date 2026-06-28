/* ==========================================================================
 *  main.c  --  Knight Rider Clock on FreeRTOS, Raspberry Pi 1 (BCM2835/ARMv6)
 *
 *  Ported to match the Pi 3/400 build: same word-based serial protocol
 *  (set / msg / run / stop / speed / leds / bright / prayer / pray /
 *   prayerclr / list / machine / time / help), same command queue + clock
 *  owner, and the same HDMI GUI (red dot-matrix clock + centered colour
 *  sweep bar + looping marquee).
 *
 *  Removed vs Pi 3/400: servo + stepper (no such hardware on the Pi 1).
 *  Requires the new drivers in this folder: fb.c/fb.h, font5x7.h
 *  (copied from the pi3_400 tree -- they are board-agnostic).
 *
 *  All tasks run at the SAME priority so FreeRTOS time-slices them fairly
 *  (a lower-priority sweep/hdmi was being starved on the ARM6_BCM2835 port).
 *  Type 'diag' on the serial console for per-task liveness counters.
 * ========================================================================== */
#include <stdint.h>
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"

#include "peripherals.h"
#include "gpio.h"
#include "uart.h"
#include "tm1637.h"
#include "max7219.h"
#include "font.h"        /* glyph()  : MAX7219 5x7 font (used inside fb.c)     */
#include "font5x7.h"     /* font_glyph()/FONT_W : marquee + HDMI text         */
#include "fb.h"
#include "qrcodegen.h"
#include "timer.h"      /* sys_us(): 74HC595 bit-bang settle */

#include "usb_dwc2.h"   



extern int bcm2835_init(void);

#define BOARD_NAME "Pi 1 (BCM2835, ARMv6)"
#define MSG_LENGTH 200

static char  upc(char c){ return (c>='a'&&c<='z') ? (char)(c-32) : c; }
static void  up_dec(uint32_t v){ char b[11]; int i=10; b[10]=0; if(!v){uart_puts("0");return;} while(v&&i){b[--i]='0'+(v%10);v/=10;} uart_puts(&b[i]); }

/* ---- command channel: parser -> clock owner --------------------------- */
typedef enum { CMD_SETTIME, CMD_MSG, CMD_NETMSG, CMD_BRIGHT, CMD_QUERY, CMD_PRAYER, CMD_PRAYCLR, CMD_LIST } cmdtype_t;
typedef struct { cmdtype_t type; int a, b, c; char text[101]; } cmd_t;
static QueueHandle_t g_cmdq;

/* ---- shared display/sweep state --------------------------------------- */
volatile uint8_t g_led_display = 0;   /* current 8-LED state (sweep or manual) */
volatile uint8_t g_led_mask    = 0;   /* manual override mask; 0 = auto sweep   */
static SemaphoreHandle_t g_spi_mutex;
volatile int g_machine   = 0;         /* 1 = suppress echo/prompt (ESP32 link)  */
char  g_marquee[200] = "KNIGHT RIDER CLOCK";
volatile int g_sweep_run = 1;
volatile int g_sweep_ms  = 80;

static const uint32_t BAR_COL[8] = {
    RGB(255,0,0),   RGB(255,128,0), RGB(255,255,0), RGB(0,255,0),
    RGB(0,255,255), RGB(0,128,255), RGB(128,0,255), RGB(255,0,255)
};

static uint32_t g_secs   = 12*3600;
static char     g_msg[MSG_LENGTH] = "RASPBERRY PI 1 FREERTOS BAREMETAL CLOCK";
static int      g_bright = MAT_INTENSITY;

/* ---- QR-on-HDMI feature (driven by the 'qr <text>' / 'qr off' command) ----
 * Parser fills g_qr_text and raises g_qr_req; the HDMI task (which owns the
 * framebuffer) does the encode + paint. Request flag set last = safe hand-off.*/
#define QR_MAXVER  10     /* up to 57x57 modules: fits URLs / long marquees     */
#define QR_QUIET   4      /* quiet-zone width in modules (QR spec minimum)       */
static uint8_t  g_qr_buf[qrcodegen_BUFFER_LEN_FOR_VERSION(QR_MAXVER)];
static uint8_t  g_qr_tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(QR_MAXVER)];
static char     g_qr_text[160];   /* payload to encode (set by the parser)      */
volatile int    g_qr_req = 0;     /* 0 idle, 1 = show pending, 2 = clear pending */
volatile int    g_qr_on  = 0;     /* 1 = QR currently on the HDMI screen         */

/* ---- Panel button: enable/disable WebSocket (netmsg) messages on displays
 * Momentary push button on GPIO21 (header pin 40) to GND, active LOW. Internal
 * pull-up is enabled (no external resistor). Each press toggles whether incoming
 * 'netmsg' commands (WebSocket bridge) reach the MAX7219 + HDMI marquee. Manual
 * 'msg' is never gated. The WebSocket link on the ESP32 keeps running either way.*/
#define BTN_MSG_PIN  21
static char     g_base_msg[MSG_LENGTH] = "RASPBERRY PI 1 FREERTOS BAREMETAL CLOCK";
volatile int    g_msg_show       = 1;  /* 1 = show WebSocket (netmsg) messages    */
volatile int    g_msg_show_dirty = 0;  /* set by button task, serviced by clock   */

/* ---- liveness counters (read via the 'diag' command) ------------------ */
static volatile uint32_t g_clk_n=0, g_tm_n=0, g_sweep_n=0, g_hdmi_n=0;
static volatile int g_fb_ok=-1;   /* -1 not tried, 0 fail, 1 ok */

/* ---- prayer schedule (set over serial by the ESP32) ------------------- */
#define NPRAYER 6
typedef struct { char name[12]; int hhmm; int valid; } prayer_t;
static prayer_t g_pray[NPRAYER];
#define PRAYER_WINDOW_MIN 10

static int imminent_prayer(uint32_t secs, char *out)
{
    int nowm = (secs/60) % 1440;
    for(int i=0;i<NPRAYER;i++){
        if(!g_pray[i].valid) continue;
        int mu = g_pray[i].hhmm - nowm;
        if(mu < 0) mu += 1440;
        if(mu <= PRAYER_WINDOW_MIN){
            int n=0; const char *nm=g_pray[i].name;
            for(int k=0; nm[k] && n<28; k++) out[n++]=upc(nm[k]);
            const char *suf=" TIME "; for(int k=0;suf[k];k++) out[n++]=suf[k];
            int hh=g_pray[i].hhmm/60, mm=g_pray[i].hhmm%60;
            out[n++]='0'+hh/10; out[n++]='0'+hh%10; out[n++]=':';
            out[n++]='0'+mm/10; out[n++]='0'+mm%10; out[n]=0;
            return 1;
        }
    }
    return 0;
}

static int build_ticker(char *dst, uint32_t secs, const char *msg)
{
    int hh=secs/3600, mm=(secs/60)%60, ss=secs%60, n=0;
    dst[n++]='0'+hh/10; dst[n++]='0'+hh%10; dst[n++]=':';
    dst[n++]='0'+mm/10; dst[n++]='0'+mm%10; dst[n++]=':';
    dst[n++]='0'+ss/10; dst[n++]='0'+ss%10;
    dst[n++]=' '; dst[n++]=' '; dst[n++]=' ';
    for(int i=0; msg[i] && n<(MSG_LENGTH + 11); i++) dst[n++]=msg[i];  /* case set by caller */
    dst[n++]=' '; dst[n++]=' '; dst[n++]=' ';
    dst[n]=0;
    return n;
}

static void render_window(const char *txt, int len, uint32_t sx)
{
    int cell = FONT_W + 1, total = len * cell;
    uint8_t fb[32];
    for(int col=0; col<32; col++){
        int p = (sx + col) % total, ci = p / cell, cw = p % cell;
        fb[col] = (cw < FONT_W) ? font_glyph(txt[ci])[cw] : 0x00;
    }
    max_render(fb);
}

/* ======================================================================= */
static void vClockOwner(void *pv)
{
    (void)pv;
    char ticker[MSG_LENGTH], pbuf[40];
    int  tlen = build_ticker(ticker, g_secs, g_msg);
    int  twpx = tlen * (FONT_W + 1);
    uint32_t sx = 0;
    TickType_t last = xTaskGetTickCount(), sec_acc = last;
    uint32_t last_built_sec = 0xFFFFFFFF;

    for(;;){
        cmd_t cmd;
        while(xQueueReceive(g_cmdq, &cmd, 0) == pdTRUE){
            switch(cmd.type){
            case CMD_SETTIME:
                g_secs = ((uint32_t)cmd.a*3600 + cmd.b*60 + cmd.c) % 86400;
                sec_acc = xTaskGetTickCount();
                uart_puts("ok time\r\n");
                break;
            case CMD_MSG: {            /* manual message: UPPERCASED, never gated */
                int i = 0;
                for(; cmd.text[i] && i < (int)sizeof(g_msg)-1; i++) g_msg[i] = upc(cmd.text[i]);
                g_msg[i] = 0;
                for(i=0; g_msg[i] && i < (int)sizeof(g_base_msg)-1; i++) g_base_msg[i] = g_msg[i];
                g_base_msg[i] = 0;                             /* remember as base marquee */
                tlen = build_ticker(ticker, g_secs, g_msg);
                twpx = tlen * (FONT_W + 1);
                sx   = 0;
                last_built_sec = 0xFFFFFFFF;
                fb_marquee_set(g_msg);                         /* HDMI ticker (uppercased) */
                break; }
            case CMD_NETMSG: {         /* WebSocket message: original case, gated by button */
                if(!g_msg_show) break;                         /* gated off -> drop, keep base */
                int i = 0;
                for(; cmd.text[i] && i < (int)sizeof(g_msg)-1; i++) g_msg[i] = cmd.text[i];
                g_msg[i] = 0;
                tlen = build_ticker(ticker, g_secs, g_msg);
                twpx = tlen * (FONT_W + 1);
                sx   = 0;
                last_built_sec = 0xFFFFFFFF;
                fb_marquee_set(g_msg);                         /* HDMI ticker (case preserved) */
                break; }
            case CMD_BRIGHT:
                g_bright = cmd.a & 0x0F; max_all(0x0A,(uint8_t)g_bright);
                uart_puts("ok bright\r\n");
                break;
            case CMD_PRAYER:
                if(cmd.a>=0 && cmd.a<NPRAYER){
                    for(int i=0;i<11;i++){ g_pray[cmd.a].name[i]=cmd.text[i]; if(!cmd.text[i]) break; }
                    g_pray[cmd.a].name[11]=0;
                    g_pray[cmd.a].hhmm = cmd.b; g_pray[cmd.a].valid = 1;
                    uart_puts("ok prayer\r\n");
                }
                break;
            case CMD_PRAYCLR:
                for(int i=0;i<NPRAYER;i++) g_pray[i].valid=0;
                uart_puts("ok prayerclr\r\n");
                break;
            case CMD_QUERY: {
                int hh=g_secs/3600, mm=(g_secs/60)%60, ss=g_secs%60;
                uart_puts("time "); up_dec(hh); uart_puts(":");
                if(mm<10)uart_puts("0"); up_dec(mm); uart_puts(":");
                if(ss<10)uart_puts("0"); up_dec(ss);
                uart_puts(" sweep="); uart_puts(g_sweep_run?"run":"stop");
                uart_puts(" spd="); up_dec(g_sweep_ms);
                uart_puts(" heap="); up_dec(xPortGetFreeHeapSize()); uart_puts("\r\n");
                break; }
            case CMD_LIST:
                for(int i=0;i<NPRAYER;i++){ if(!g_pray[i].valid) continue;
                    uart_puts("  ["); up_dec(i); uart_puts("] "); uart_puts(g_pray[i].name);
                    uart_puts(" "); up_dec(g_pray[i].hhmm/60); uart_puts(":");
                    if(g_pray[i].hhmm%60<10)uart_puts("0"); up_dec(g_pray[i].hhmm%60); uart_puts("\r\n"); }
                break;
            }
            last_built_sec = 0xFFFFFFFF;   /* force rebuild */
        }

        /* panel button toggled the WebSocket-message gate */
        if(g_msg_show_dirty){
            g_msg_show_dirty = 0;
            if(g_msg_show){
                uart_puts("msg display: ON\r\n");
            } else {
                int i=0; for(; g_base_msg[i] && i<(int)sizeof(g_msg)-1; i++) g_msg[i]=g_base_msg[i];
                g_msg[i]=0;
                tlen = build_ticker(ticker, g_secs, g_msg);
                twpx = tlen * (FONT_W + 1);
                sx = 0;
                last_built_sec = 0xFFFFFFFF;
                fb_marquee_set(g_msg);
                uart_puts("msg display: OFF\r\n");
            }
        }

        TickType_t now = xTaskGetTickCount();
        while((TickType_t)(now - sec_acc) >= pdMS_TO_TICKS(1000)){
            sec_acc += pdMS_TO_TICKS(1000);
            g_secs = (g_secs + 1) % 86400;
        }

        if(g_secs != last_built_sec){
            const char *mt = imminent_prayer(g_secs, pbuf) ? pbuf : g_msg;
            { int i=0; for(;mt[i]&&i<39;i++) g_marquee[i]=mt[i]; g_marquee[i]=0; }
            tlen = build_ticker(ticker, g_secs, mt);
            twpx = tlen * (FONT_W + 1);
            if(sx >= (uint32_t)twpx) sx = 0;
            last_built_sec = g_secs;
        }

        xSemaphoreTake(g_spi_mutex, portMAX_DELAY);
        render_window(ticker, tlen, sx);
        xSemaphoreGive(g_spi_mutex);
        sx++; if(sx >= (uint32_t)twpx) sx = 0;

        g_clk_n++;
        vTaskDelayUntil(&last, pdMS_TO_TICKS(70));
    }
}

/* ======================================================================= */
static void vTm1637(void *pv)
{
    (void)pv;
    for(;;){
        uint32_t sc = g_secs;
        int hh = sc/3600, mm = (sc/60)%60;
        int colon = ((xTaskGetTickCount() / pdMS_TO_TICKS(500)) & 1);
        tm_time(hh, mm, colon);
        g_tm_n++;
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

/* ======================================================================= */
/* ======================================================================= */
/* LED-sweep output backend (build-time switchable)                          */
/*   make                -> 74HC595 shift register, 3 GPIOs (default)         */
/*   make LEDSWEEP=FULL   -> 8 direct GPIOs (original wiring)                 */
/* bit k of 'bits' = LED k (bit0 = first LED).                               */
#ifdef LEDSWEEP_FULL
static const uint8_t SWEEP[8] = { 4, 17, 18, 27, 22, 23, 24, 25 };
static void led_out_init(void){
    for(int k=0;k<8;k++){ gpio_set_function(SWEEP[k],GPIO_FUNC_OUTPUT); gpio_clear(1u<<SWEEP[k]); }
}
static void led_out_write(uint8_t bits){
    uint32_t set=0, clr=0;
    for(int k=0;k<8;k++){ if(bits&(1u<<k)) set|=(1u<<SWEEP[k]); else clr|=(1u<<SWEEP[k]); }
    gpio_set(set); gpio_clear(clr);
}
#else
/* 74HC595: DATA->DS(14), CLOCK->SHCP(11), LATCH->STCP(12); OE(13)->GND,
 * SRCLR/MR(10)->3V3; outputs Q0..Q7 -> LEDs. */
#define HC595_DATA   17
#define HC595_CLOCK  27
#define HC595_LATCH  22
static inline void hc595_settle(void){ uint64_t t=sys_us(); while(sys_us()-t < 2){} } /* ~1-2us */
static void led_out_init(void){
    gpio_set_function(HC595_DATA,  GPIO_FUNC_OUTPUT);
    gpio_set_function(HC595_CLOCK, GPIO_FUNC_OUTPUT);
    gpio_set_function(HC595_LATCH, GPIO_FUNC_OUTPUT);
    gpio_clear((1u<<HC595_DATA)|(1u<<HC595_CLOCK)|(1u<<HC595_LATCH));
}
static void led_out_write(uint8_t bits){
    for(int k=7;k>=0;k--){                       /* MSB first -> Qk = bit k */
        if(bits&(1u<<k)) gpio_set(1u<<HC595_DATA); else gpio_clear(1u<<HC595_DATA);
        hc595_settle();
        gpio_set(1u<<HC595_CLOCK);  hc595_settle();
        gpio_clear(1u<<HC595_CLOCK);
    }
    gpio_set(1u<<HC595_LATCH);  hc595_settle();
    gpio_clear(1u<<HC595_LATCH);
}
#endif

/* ======================================================================= */
static void vSweep(void *pv)
{
    (void)pv; int i=0, dir=1;
    led_out_init();   /* init inside the task (matches the working Pi 1 build) */
    for(;;){
        g_sweep_n++;
        uint8_t mask = g_led_mask;
        if(mask){
            led_out_write(mask);
            g_led_display = mask;
            vTaskDelay(pdMS_TO_TICKS(60));
        } else if(g_sweep_run){
            uint8_t bits = (uint8_t)(1u<<i);
            led_out_write(bits);
            g_led_display = bits;
            i += dir; if(i>=7) dir=-1; else if(i<=0) dir=1;
            int ms = g_sweep_ms; if(ms<20) ms=20; if(ms>1000) ms=1000;
            vTaskDelay(pdMS_TO_TICKS(ms));
        } else {
            led_out_write(0);
            g_led_display = 0;
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

/* ======================================================================= */
/* ---- QR painters: shared encode + blit, then full-screen and small modes -- */
/* Encode g_qr_text into g_qr_buf; returns module count n, or 0 on failure.   */
static int qr_encode(void)
{
    if(!qrcodegen_encodeText(g_qr_text, g_qr_tmp, g_qr_buf,
                             qrcodegen_Ecc_MEDIUM, qrcodegen_VERSION_MIN, QR_MAXVER,
                             qrcodegen_Mask_AUTO, true))
        return 0;                                   /* text too long for QR_MAXVER */
    return qrcodegen_getSize(g_qr_buf);
}

/* Paint the encoded QR as black modules on a white quiet-zone field: top-left  */
/* at (ox,oy), 's' pixels per module. Scannable by any phone camera.            */
static void qr_blit(int ox, int oy, int s, int n)
{
    const uint32_t WHITE = RGB(255,255,255), BLACK = RGB(0,0,0);
    int side = (n + 2*QR_QUIET) * s;
    fb_fill(ox, oy, side, side, WHITE);             /* white field incl. quiet zone*/
    for(int y=0; y<n; y++)
        for(int x=0; x<n; x++)
            if(qrcodegen_getModule(g_qr_buf, x, y))
                fb_fill(ox + (QR_QUIET+x)*s, oy + (QR_QUIET+y)*s, s, s, BLACK);
}

/* Full-screen QR: clears the screen and centers the code with a caption.       */
static int qr_paint_full(void)
{
    int n = qr_encode(); if(!n) return 0;
    int units = n + 2*QR_QUIET;
    int W = (int)fb_width(), H = (int)fb_height();
    int avail = (W < H ? W : H) - 96;
    int s = avail / units; if(s < 2) s = 2;
    int side = units * s;
    int ox = (W - side)/2;
    int oy = (H - side)/2 - 24; if(oy < 0) oy = 0;

    fb_clear(RGB(0,0,0));
    qr_blit(ox, oy, s, n);
    char cap[49]; int ci=0;
    for(; g_qr_text[ci] && ci<48; ci++) cap[ci]=g_qr_text[ci];
    cap[ci]=0;
    fb_text((W - fb_text_w(cap,2))/2, oy + side + 18, cap, 2, RGB(0,220,255));
    return 1;
}

/* Small QR: lives in the empty band between the colour bar (~y348) and the     */
/* marquee (y600), so the clock dashboard keeps running around it.              */
#define QR_BAND_TOP 360
#define QR_BAND_BOT 590
static void qr_band_clear(void)
{
    fb_fill(0, QR_BAND_TOP, (int)fb_width(), QR_BAND_BOT - QR_BAND_TOP, RGB(0,0,0));
}
static int qr_paint_small(void)
{
    int n = qr_encode(); if(!n) return 0;
    int units = n + 2*QR_QUIET;
    int W = (int)fb_width();
    int availh = QR_BAND_BOT - QR_BAND_TOP;
    int s = availh / units; if(s < 2) s = 2;
    int side = units * s;
    int ox = (W - side)/2;
    int oy = QR_BAND_TOP + (availh - side)/2; if(oy < QR_BAND_TOP) oy = QR_BAND_TOP;

    qr_band_clear();                                /* erase any previous small QR */
    qr_blit(ox, oy, s, n);
    return 1;
}

/* ======================================================================= */
static void vHdmi(void *pv)
{
    (void)pv;
    if(!fb_init(1024,768)){ g_fb_ok=0; uart_puts("hdmi: fb_init FAILED\r\n"); vTaskDelete(NULL); return; }
    g_fb_ok=1;

    const uint32_t BLACK=RGB(0,0,0);
    const int BAR_Y=260, CELL_W=60, CELL_H=88, GAP=24;
    fb_clear(BLACK);
    char prev[9]={0}; int prev_sweep=-2;
    fb_marquee_set(g_msg);

    for(;;){
        /* QR feature: full-screen / small (overlay) variants, plus clear        */
        int qr_rq = g_qr_req;
        if(qr_rq){
            g_qr_req = 0;
            if(qr_rq == 2){                          /* hide whatever QR is shown    */
                if(g_qr_on == 1){                    /* was full screen: rebuild GUI */
                    fb_clear(BLACK); fb_clock_reset();
                    prev[0] = 0; prev_sweep = -2; fb_marquee_set(g_msg);
                } else if(g_qr_on == 2){             /* was small: clear its band    */
                    qr_band_clear();
                }
                g_qr_on = 0;
            } else if(qr_rq == 1){                   /* full-screen QR               */
                if(qr_paint_full()) g_qr_on = 1;
                else uart_puts("qr: encode failed (text too long?)\r\n");
            } else if(qr_rq == 3){                   /* small QR overlaid on the GUI */
                if(g_qr_on == 1){                    /* coming from full: restore GUI */
                    fb_clear(BLACK); fb_clock_reset();
                    prev[0] = 0; prev_sweep = -2; fb_marquee_set(g_msg);
                }
                if(qr_paint_small()) g_qr_on = 2;
                else uart_puts("qr: encode failed (text too long?)\r\n");
            }
        }
        if(g_qr_on == 1){ g_hdmi_n++; vTaskDelay(pdMS_TO_TICKS(50)); continue; }  /* full QR holds, GUI hidden */

        uint32_t s=g_secs; int hh=s/3600, mm=(s/60)%60, ss=s%60; char t[9];
        t[0]='0'+hh/10; t[1]='0'+hh%10; t[2]=':'; t[3]='0'+mm/10; t[4]='0'+mm%10; t[5]=':';
        t[6]='0'+ss/10; t[7]='0'+ss%10; t[8]=0;
        int diff=0; for(int k=0;k<9;k++) if(t[k]!=prev[k]) diff=1;
        if(diff){ fb_clock_big(t, RGB(255,0,0), BLACK); for(int k=0;k<9;k++)prev[k]=t[k]; }

        fb_marquee_draw(600, 7, 8, RGB(255,170,0), BLACK);   /* step 4->8 */

        int disp=g_led_display;
        if(disp!=prev_sweep){
            int total = 8*CELL_W + 7*GAP;
            int x0    = ((int)fb_width() - total)/2;
            for(int c=0;c<8;c++){
                uint32_t col = BAR_COL[c];
                uint32_t off = RGB(((col>>16)&0xFF)/6, ((col>>8)&0xFF)/6, (col&0xFF)/6);
                fb_fill(x0 + c*(CELL_W+GAP), BAR_Y, CELL_W, CELL_H, (disp&(1<<c)) ? col : off);
            }
            prev_sweep=disp;
        }
        g_hdmi_n++;
        vTaskDelay(pdMS_TO_TICKS(25));   /* 40->25ms : faster marquee */
    }
}

/* ======================================================================= */
/* command parser  (servo/stepper removed: no such hardware on the Pi 1)   */
static int seq(const char*a,const char*b){ int i=0; for(;a[i]&&b[i];i++) if(a[i]!=b[i])return 0; return a[i]==b[i]; }
static const char *PROMPT = "krclock> ";
static int parse_uint(const char **p){ int v=0; while(**p>='0'&&**p<='9'){ v=v*10+(**p-'0'); (*p)++; } return v; }
static void skip_sp(const char **p){ while(**p==' ') (*p)++; }
static int word(const char **p, char *out, int max){ int n=0; while(**p && **p!=' ' && n<max-1){ out[n++]=*(*p)++; } out[n]=0; return n; }

static void print_help(void)
{
    uart_puts("commands:\r\n");
    uart_puts("  set HH:MM[:SS]          set/sync clock (NTP)\r\n");
    uart_puts("  time                   show status\r\n");
    uart_puts("  msg <text>             marquee message (shown UPPERCASE, always)\r\n");
    uart_puts("  netmsg <text>          marquee message (orig case; button can gate)\r\n");
    uart_puts("  qrfull <text>          full-screen QR on HDMI (alias: qr)\r\n");
    uart_puts("  qrsmall <text>         small QR between bar & marquee\r\n");
    uart_puts("  qr off                 hide QR, back to clock\r\n");
    uart_puts("  run | stop             sweep on/off\r\n");
    uart_puts("  speed <20-1000>        sweep frame ms\r\n");
    uart_puts("  leds <0-255>           manual LED mask (0=auto sweep)\r\n");
    uart_puts("  bright <0-15>          MAX7219 intensity\r\n");
    uart_puts("  prayer <0-5> <NAME> <HH:MM>   set a prayer slot\r\n");
    uart_puts("  pray <NAME> <HH:MM>    announce a prayer now\r\n");
    uart_puts("  prayerclr | list       clear / list prayers\r\n");
    uart_puts("  machine on|off         suppress echo/prompt (ESP32 link)\r\n");
    uart_puts("  diag                   task liveness counters\r\n");
}

/* Shared 'qr' handling: 'off'/'clear' hides any QR; otherwise the rest of the
 * line is the payload. showreq = 1 (full screen) or 3 (small overlay).        */
static void qr_request(const char *p, int showreq)
{
    const char *q = p; char a[8]; word(&q, a, sizeof a);
    if(seq(a,"off") || seq(a,"clear")){ g_qr_req = 2; uart_puts("ok qr off\r\n"); return; }
    int i=0; while(p[i] && i<(int)sizeof(g_qr_text)-1){ g_qr_text[i]=p[i]; i++; }
    g_qr_text[i]=0;
    if(!i){ uart_puts("err empty\r\n"); return; }
    g_qr_req = showreq; uart_puts("ok qr\r\n");
}

void parse_line(char *line)
{
    const char *p = line; skip_sp(&p);
    char cw[16]; if(!word(&p,cw,sizeof cw)) return; skip_sp(&p);

    if(seq(cw,"set")){
        cmd_t c; c.type=CMD_SETTIME;
        c.a=parse_uint(&p); if(*p==':')p++; c.b=parse_uint(&p);
        c.c=0; if(*p==':'){ p++; c.c=parse_uint(&p); }
        if(c.a>23||c.b>59||c.c>59){ uart_puts("err range\r\n"); return; }
        xQueueSend(g_cmdq,&c,0);
    } else if(seq(cw,"time")){
        cmd_t c; c.type=CMD_QUERY; xQueueSend(g_cmdq,&c,0);
    } else if(seq(cw,"msg")){
        cmd_t c; c.type=CMD_MSG; int i=0; while(*p && i<(int)sizeof(c.text)-1) c.text[i++]=*p++; c.text[i]=0;
        if(!i){ uart_puts("err empty\r\n"); return; } xQueueSend(g_cmdq,&c,0);
    } else if(seq(cw,"netmsg")){
        cmd_t c; c.type=CMD_NETMSG; int i=0; while(*p && i<(int)sizeof(c.text)-1) c.text[i++]=*p++; c.text[i]=0;
        if(!i){ uart_puts("err empty\r\n"); return; } xQueueSend(g_cmdq,&c,0);
    } else if(seq(cw,"qr") || seq(cw,"qrfull")){
        qr_request(p, 1);          /* full-screen QR (qr = alias for qrfull)        */
    } else if(seq(cw,"qrsmall")){
        qr_request(p, 3);          /* small QR in the band between bar and marquee  */
    } else if(seq(cw,"run")){
        g_sweep_run=1; uart_puts("ok run\r\n");
    } else if(seq(cw,"stop")){
        g_sweep_run=0; uart_puts("ok stop\r\n");
    } else if(seq(cw,"speed")){
        int v=parse_uint(&p); if(v<20)v=20; if(v>1000)v=1000; g_sweep_ms=v; uart_puts("ok speed\r\n");
    } else if(seq(cw,"leds")){
        int m=parse_uint(&p); g_led_mask=(uint8_t)(m&0xFF); uart_puts("ok leds\r\n");
    } else if(seq(cw,"bright")){
        cmd_t c; c.type=CMD_BRIGHT; c.a=parse_uint(&p);
        if(c.a>15){ uart_puts("err range\r\n"); return; } xQueueSend(g_cmdq,&c,0);
    } else if(seq(cw,"prayer")){
        cmd_t c; c.type=CMD_PRAYER; c.a=parse_uint(&p); skip_sp(&p);
        word(&p,c.text,sizeof c.text); skip_sp(&p);
        int hh=parse_uint(&p); if(*p==':')p++; int mm=parse_uint(&p);
        if(c.a<0||c.a>=NPRAYER||hh>23||mm>59){ uart_puts("err range\r\n"); return; }
        c.b=hh*60+mm; xQueueSend(g_cmdq,&c,0);
    } else if(seq(cw,"pray")){
        char nm[16]; word(&p,nm,sizeof nm); skip_sp(&p);
        int hh=parse_uint(&p); if(*p==':')p++; int mm=parse_uint(&p);
        if(hh>23||mm>59){ uart_puts("err range\r\n"); return; }
        cmd_t c; c.type=CMD_MSG; int n=0;
        for(int k=0;nm[k]&&n<28;k++) c.text[n++]=nm[k];
        const char *suf=" TIME "; for(int k=0;suf[k];k++) c.text[n++]=suf[k];
        c.text[n++]='0'+hh/10; c.text[n++]='0'+hh%10; c.text[n++]=':';
        c.text[n++]='0'+mm/10; c.text[n++]='0'+mm%10; c.text[n]=0;
        xQueueSend(g_cmdq,&c,0);
    } else if(seq(cw,"prayerclr")){
        cmd_t c; c.type=CMD_PRAYCLR; xQueueSend(g_cmdq,&c,0);
    } else if(seq(cw,"list")){
        cmd_t c; c.type=CMD_LIST; xQueueSend(g_cmdq,&c,0);
    } else if(seq(cw,"machine")){
        char a[8]; word(&p,a,sizeof a);
        if(seq(a,"on")){ g_machine=1; uart_puts("ok machine\r\n"); }
        else { g_machine=0; uart_puts("ok human\r\n"); uart_puts(PROMPT); }
    } else if(seq(cw,"diag")){
        uart_puts("clk=");   up_dec(g_clk_n);
        uart_puts(" tm=");   up_dec(g_tm_n);
        uart_puts(" sweep=");up_dec(g_sweep_n);
        uart_puts(" hdmi="); up_dec(g_hdmi_n);
        uart_puts(" fb=");   up_dec((uint32_t)(g_fb_ok<0?9:g_fb_ok));
        uart_puts("(");      up_dec(fb_width()); uart_puts("x"); up_dec(fb_height()); uart_puts(")");
        uart_puts(" heap="); up_dec(xPortGetFreeHeapSize());
        uart_puts(" run=");  up_dec((uint32_t)g_sweep_run);
        uart_puts(" mask="); up_dec((uint32_t)g_led_mask);
        uart_puts("\r\n");
    } else if(seq(cw,"help")){
        print_help();
    } else {
        uart_puts("? help\r\n");
    }
}

static void vUartCmd(void *pv)
{
    (void)pv;
    char line[128]; int li=0;
    while(uart_getc_nb()>=0){}
    uart_puts("\r\n"); print_help(); uart_puts(PROMPT);
    for(;;){
        int ch, active=0;
        while((ch = uart_getc_nb()) >= 0){
            active = 1;
            if(ch=='\r' || ch=='\n'){
                if(!g_machine) uart_puts("\r\n");
                line[li]=0; parse_line(line); li=0;
                if(!g_machine) uart_puts(PROMPT);
            } else if(ch==8 || ch==127){
                if(li>0){ li--; if(!g_machine) uart_puts("\b \b"); }
            } else if(li < (int)sizeof(line)-1){
                line[li++]=(char)ch; if(!g_machine) uart_putc((char)ch);
            }
        }
        if(active) taskYIELD();
        else       vTaskDelay(pdMS_TO_TICKS(1));
    }
}

/* ======================================================================= */
/* ======================================================================= */
/* Panel button: debounced active-LOW on BTN_MSG_PIN with internal pull-up.   */
static void vButton(void *pv)
{
    (void)pv;
    gpio_set_function(BTN_MSG_PIN, GPIO_FUNC_INPUT);
    gpio_set_pull(BTN_MSG_PIN, GPIO_PULL_UP);   /* idle HIGH; button pulls to GND */
    int last = 1, stable = 1, cnt = 0;
    for(;;){
        int raw = gpio_read(BTN_MSG_PIN);
        if(raw != last){ last = raw; cnt = 0; }
        else if(cnt < 3){ cnt++; }
        if(cnt == 3 && raw != stable){
            stable = raw;
            if(stable == 0){ g_msg_show ^= 1; g_msg_show_dirty = 1; }
        }
        vTaskDelay(pdMS_TO_TICKS(15));
    }
}



static void vUsbTest(void *pv) {
    (void)pv;
    uart_puts("\r\n=== USB Ethernet bring-up test (Pi 1) ===\r\n");
    int r = usbnet_init();          /* DWC2 host -> hub -> SMSC9512 -> link */
    if (r > 0) uart_puts("USB TEST: link UP, driver OK\r\n");
    else       uart_puts("USB TEST: bring-up FAILED (see log)\r\n");
    for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
}













/* ======================================================================= */
int main(void)
{
    bcm2835_init();
    uart_init();
    uart_puts("\r\n=== Knight Rider Clock  (FreeRTOS, " BOARD_NAME ") ===\r\n");
    tm1637_init(); max_init();

    uart_puts("\r\n=== USB bring-up (pre-scheduler) ===\r\n");
    int usb = usbnet_init();
    uart_puts(usb > 0 ? "USB TEST: link UP\r\n" : "USB TEST: FAILED (see log)\r\n");

    g_spi_mutex = xSemaphoreCreateMutex();
    g_cmdq = xQueueCreate(8, sizeof(cmd_t));
    if(!g_cmdq){ uart_puts("queue alloc failed\r\n"); for(;;){} }

    /* displays share priority 1 (fair round-robin); the UART task sits one
       level up so it drains the 16-byte RX FIFO every ~1ms and never loses a
       burst -- it sleeps when idle, so it still can't starve the displays. */
    xTaskCreate(vClockOwner,"clock", 1024, NULL, 1, NULL);
    xTaskCreate(vTm1637,    "tm",     384, NULL, 1, NULL);
    xTaskCreate(vSweep,     "sweep",  512, NULL, 1, NULL);
    xTaskCreate(vButton,    "btn",    512, NULL, 1, NULL);
    xTaskCreate(vHdmi,      "hdmi",  1024, NULL, 1, NULL);
    xTaskCreate(vUartCmd,   "uart",  1024, NULL, 2, NULL);
    
    void net_start(void);   /* defined in mongoose_glue.c */
    net_start();
    vTaskStartScheduler();
    while(1);
    return 0;
}
