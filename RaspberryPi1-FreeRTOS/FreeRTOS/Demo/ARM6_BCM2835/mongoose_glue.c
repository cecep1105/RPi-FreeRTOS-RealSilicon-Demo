/* Glue between Mongoose's built-in TCP/IP stack and the bare-metal GENET driver.
 *
 * Provides: nostdlib libc/malloc shims (weak, so they never collide with
 * kstring.c or the toolchain), mg_millis(), mg_random(), a UART log sink,
 * the mg_tcpip_driver wrapping genet_net_*, and a FreeRTOS task that runs
 * DHCP and mg_mgr_poll().
 */
#include "mongoose.h"
#include "genet.h"
#include "uart.h"
#include "timer.h"

#include "FreeRTOS.h"
#include "task.h"

/* the command parser from main.c (UART console path); now non-static */
extern void parse_line(char *line);
extern char g_marquee[];   /* live HDMI marquee text (main.c) */

/* ---- Web dashboard authentication (HTTP Basic) ----------------------------
 * Basic auth base64-encodes (does NOT encrypt) the credentials on every
 * request, so it only gates casual access on a trusted LAN -- it is not a
 * substitute for TLS. Three ways to set the credentials:
 *   (a) edit the defaults below, or
 *   (b) override from the Makefile (GNU make), e.g.:
 *         CFLAGS +=-DWEB_USER='"alice"' -DWEB_PASS='"s3cret"'
 *       (on Windows make the quoting is fragile -- editing the defaults
 *        below is the reliable route)
 * Set WEB_AUTH to 0 to remove the login prompt entirely. */
#ifndef WEB_AUTH
#define WEB_AUTH 1
#endif
#ifndef WEB_USER
#define WEB_USER "admin"
#endif
#ifndef WEB_PASS
#define WEB_PASS "pi400"
#endif

/* ---- Network services moved onto the Pi (were on the ESP32) --------------
 * NTP keeps the clock in sync; the WebSocket client receives relay frames and
 * feeds them in as `netmsg ...`. Both reuse parse_line()/g_cmdq, so they act
 * exactly like the UART/ESP32 command path.
 *   TZ_OFFSET_MIN : local = UTC + this many minutes (Jakarta = +7h = 420).
 *   NTP_URL       : needs working DNS (the gateway must route to the internet,
 *                   or point this at a reachable NTP host/IP).
 *   WS_URL        : your Python relay backend; set it, then flip ENABLE_WS=1. */
#ifndef ENABLE_NTP
#define ENABLE_NTP 1
#endif
#ifndef NTP_URL
#define NTP_URL "udp://time.google.com:123"
#endif
#ifndef DNS_URL
#define DNS_URL ""        /* empty => resolve via the DHCP gateway (your router) */
#endif
#ifndef TZ_OFFSET_MIN
#define TZ_OFFSET_MIN 420
#endif
#ifndef ENABLE_WS
#define ENABLE_WS 1
#endif

/* ---- Static-IP fallback ------------------------------------------------
 *  If no DHCP lease arrives within STATIC_DHCP_WAIT_MS, the interface comes
 *  up on this fixed address instead. Edit these to match your network; set
 *  STATIC_FALLBACK 0 for DHCP-only. (No effect once DHCP succeeds.) */
#ifndef STATIC_FALLBACK
#define STATIC_FALLBACK 1
#endif
#ifndef STATIC_DHCP_WAIT_MS
#define STATIC_DHCP_WAIT_MS 15000
#endif
#define STATIC_IP4(a,b,c,d) ((uint32_t)(a) | ((uint32_t)(b)<<8) | \
                             ((uint32_t)(c)<<16) | ((uint32_t)(d)<<24))
#ifndef STATIC_IP
#define STATIC_IP   STATIC_IP4(192,168,30,50)
#endif
#ifndef STATIC_MASK
#define STATIC_MASK STATIC_IP4(255,255,255,0)
#endif
#ifndef STATIC_GW
#define STATIC_GW   STATIC_IP4(192,168,30,254)
#endif
#ifndef WS_URL
#define WS_URL "ws://172.16.10.36:4001/ws/netmon/?timeid=1234567890"
#endif

/* ----- libc shims (weak: used only if nothing else provides them) ----- */
#define WEAK __attribute__((weak))

WEAK void *memcpy(void *d, const void *s, size_t n) {
    uint8_t *a = d; const uint8_t *b = s; while (n--) *a++ = *b++; return d;
}
WEAK int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *x = a, *y = b; while (n--) { if (*x != *y) return *x - *y; x++; y++; } return 0;
}
WEAK int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; } return (unsigned char)*a - (unsigned char)*b;
}
WEAK int strncmp(const char *a, const char *b, size_t n) {
    while (n && *a && *a == *b) { a++; b++; n--; }
    return n ? (unsigned char)*a - (unsigned char)*b : 0;
}
WEAK char *strrchr(const char *s, int c) {
    const char *last = 0; do { if (*s == (char)c) last = s; } while (*s++); return (char *)last;
}
WEAK char *strstr(const char *h, const char *n) {
    if (!*n) return (char *)h;
    for (; *h; h++) { const char *a = h, *b = n;
        while (*a && *b && *a == *b) { a++; b++; }
        if (!*b) return (char *)h; }
    return 0;
}
WEAK size_t strnlen(const char *s, size_t m) { size_t i = 0; while (i < m && s[i]) i++; return i; }
WEAK char *strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)) {} return r; }
WEAK char *strncpy(char *d, const char *s, size_t n) {
    char *r = d; while (n && (*d = *s)) { d++; s++; n--; } while (n--) *d++ = 0; return r;
}
WEAK char *strcat(char *d, const char *s) { char *r = d; while (*d) d++; while ((*d++ = *s++)) {} return r; }
WEAK void *memchr(const void *p, int c, size_t n) {
    const uint8_t *q = p; while (n--) { if (*q == (uint8_t)c) return (void *)q; q++; } return 0;
}
WEAK long strtol(const char *s, char **end, int base) {
    const char *p = s; long sign = 1, val = 0;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '+') p++; else if (*p == '-') { sign = -1; p++; }
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { base = 16; p += 2; }
    else if (base == 0 && p[0] == '0') { base = 8; p++; }
    else if (base == 0) base = 10;
    for (;;) {
        int c = *p, d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'z') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'Z') d = c - 'A' + 10;
        else break;
        if (d >= base) break;
        val = val * base + d; p++;
    }
    if (end) *end = (char *)p;
    return sign * val;
}
WEAK int atoi(const char *s) { return (int)strtol(s, 0, 10); }
WEAK char *strdup(const char *s) {
    size_t n = strlen(s) + 1; char *p = malloc(n);
    if (p) for (size_t i = 0; i < n; i++) p[i] = s[i];
    return p;
}
static uint32_t s_rng = 1;
WEAK void srand(unsigned seed) { s_rng = seed ? seed : 1u; }
WEAK int  rand(void) { s_rng = s_rng * 1103515245u + 12345u; return (int)((s_rng >> 16) & 0x7FFFu); }
/* sscanf is referenced only by mg_check_ip_acl (ACL parsing), which we never
 * invoke -- a stub satisfies the linker and is never called at runtime. */
WEAK int sscanf(const char *str, const char *fmt, ...) { (void)str; (void)fmt; return -1; }

/* ----- malloc family on top of the FreeRTOS heap ----- */
WEAK void *malloc(size_t n) { return pvPortMalloc(n); }
WEAK void  free(void *p)    { vPortFree(p); }
WEAK void *calloc(size_t a, size_t b) {
    size_t n = a * b; void *p = pvPortMalloc(n);
    if (p) { uint8_t *q = p; for (size_t i = 0; i < n; i++) q[i] = 0; } return p;
}
WEAK void *realloc(void *p, size_t n) {
    /* simple grow-only realloc: allocate new, copy, free old (size unknown -> copy n) */
    if (!p) return pvPortMalloc(n);
    if (!n) { vPortFree(p); return 0; }
    void *q = pvPortMalloc(n);
    if (q) { uint8_t *a = q; const uint8_t *b = p; for (size_t i = 0; i < n; i++) a[i] = b[i]; vPortFree(p); }
    return q;
}
WEAK void abort(void) { for (;;) {} }
WEAK int putchar(int c) { uart_putc((char)c); return c; }

/* ----- time base + RNG that Mongoose needs ----- */
uint64_t mg_millis(void) { return sys_us() / 1000ULL; }

bool mg_random(void *buf, size_t len) {
    static uint64_t s = 0;
    if (!s) s = sys_us() ^ 0x9E3779B97F4A7C15ULL;
    uint8_t *p = buf;
    for (size_t i = 0; i < len; i++) {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;     /* xorshift64 */
        p[i] = (uint8_t)(s ^ sys_us());
    }
    return true;
}

/* ====================== mg_tcpip driver glue ========================= */
static bool drv_init(struct mg_tcpip_if *ifp) {
    (void)ifp;
    genet_net_init();          /* idempotent: hardware already up from boot probe */
    return true;
}
static size_t drv_tx(const void *buf, size_t len, struct mg_tcpip_if *ifp) {
    (void)ifp;
    return genet_net_tx(buf, len) ? len : 0;
}
static size_t drv_rx(void *buf, size_t len, struct mg_tcpip_if *ifp) {
    (void)ifp;
    return genet_net_rx(buf, len);
}
static bool drv_poll(struct mg_tcpip_if *ifp, bool s1) {
    (void)ifp;
    return s1 ? (genet_net_up() != 0) : false;
}

static struct mg_tcpip_driver s_genet_driver = {
    drv_init, drv_tx, drv_rx, drv_poll,
};

/* ====================== the network task ============================ */
static struct mg_mgr      s_mgr;   /* kept off the task stack */
static struct mg_tcpip_if s_mif;

static void put_u8(uint8_t v) {
    char b[4]; int i = 3; b[3] = 0;
    do { b[--i] = (char)('0' + v % 10); v /= 10; } while (v && i);
    uart_puts(&b[i]);
}
static void put_ip(uint32_t ip) {       /* network-order octets, byte 0..3 */
    const uint8_t *o = (const uint8_t *) &ip;
    put_u8(o[0]); uart_putc('.'); put_u8(o[1]); uart_putc('.');
    put_u8(o[2]); uart_putc('.'); put_u8(o[3]);
}

static const char DASH[] =
  "<!doctype html><html lang=en><head><meta charset=utf-8>\n"
  "<meta name=viewport content='width=device-width,initial-scale=1'>\n"
  "<title>PI400 CONSOLE</title>\n"
  "<style>\n"
  ":root{\n"
  "  --bg:#0a0e14; --panel:#121822; --panel2:#0e141d; --line:#1f2a38;\n"
  "  --ink:#d7e0ea; --muted:#6b7787; --red:#ff2d2d; --ok:#39d98a; --amber:#ffb020; --blue:#4ea1ff;\n"
  "}\n"
  "*{box-sizing:border-box}\n"
  "body{margin:0;background:var(--bg);color:var(--ink);\n"
  "  font:14px/1.45 system-ui,-apple-system,Segoe UI,Roboto,sans-serif;\n"
  "  -webkit-font-smoothing:antialiased}\n"
  ".mono{font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,\"Liberation Mono\",monospace}\n"
  "header{padding:18px 20px 14px;border-bottom:1px solid var(--line);background:var(--panel2)}\n"
  ".brand{display:flex;align-items:baseline;gap:12px;flex-wrap:wrap}\n"
  "h1{margin:0;font:600 18px/1 ui-monospace,Menlo,Consolas,monospace;letter-spacing:.14em}\n"
  ".brand .sub{color:var(--muted);font-size:12px;letter-spacing:.18em}\n"
  ".scan{margin-top:12px;height:6px;border-radius:3px;background:#180a0a;overflow:hidden;\n"
  "  border:1px solid #2a1212}\n"
  ".scan i{display:block;height:100%;width:34%;border-radius:3px;\n"
  "  background:linear-gradient(90deg,transparent,var(--red),#ff8a8a,var(--red),transparent);\n"
  "  filter:drop-shadow(0 0 6px var(--red));animation:sweep 1.6s ease-in-out infinite alternate}\n"
  "@keyframes sweep{from{margin-left:-2%}to{margin-left:68%}}\n"
  ".status{margin-top:10px;color:var(--muted);font-size:12px}\n"
  ".status b{color:var(--ok)}\n"
  "main{max-width:1100px;margin:0 auto;padding:20px;\n"
  "  display:grid;gap:14px;grid-template-columns:repeat(auto-fill,minmax(290px,1fr))}\n"
  ".card{background:var(--panel);border:1px solid var(--line);border-radius:10px;padding:14px 14px 16px}\n"
  ".card h2{margin:0 0 11px;font:600 11px/1 ui-monospace,Menlo,Consolas,monospace;\n"
  "  letter-spacing:.2em;color:var(--muted);display:flex;align-items:center;gap:8px}\n"
  ".card h2::before{content:'';width:7px;height:7px;border-radius:2px;background:var(--blue)}\n"
  ".row{display:flex;gap:8px;margin:8px 0;align-items:center;flex-wrap:wrap}\n"
  ".row.tight{margin:6px 0}\n"
  "label{font-size:12px;color:var(--muted);min-width:38px}\n"
  "input{flex:1;min-width:0;background:var(--panel2);border:1px solid var(--line);color:var(--ink);\n"
  "  border-radius:7px;padding:8px 10px;font:13px ui-monospace,Menlo,Consolas,monospace}\n"
  "input:focus{outline:none;border-color:var(--blue);box-shadow:0 0 0 2px rgba(78,161,255,.15)}\n"
  "input::placeholder{color:#465162}\n"
  "button{background:#1b2636;border:1px solid #2a3a50;color:var(--ink);border-radius:7px;\n"
  "  padding:8px 12px;font:600 12px ui-monospace,Menlo,Consolas,monospace;cursor:pointer;\n"
  "  letter-spacing:.03em;transition:.12s}\n"
  "button:hover{background:#243349;border-color:var(--blue)}\n"
  "button:active{transform:translateY(1px)}\n"
  "button.go{background:#16324a;border-color:#27557d}\n"
  "button.go:hover{background:#1d4a6b}\n"
  "button.stop{border-color:#5a2330;color:#ff9a9a}\n"
  "button.stop:hover{background:#2a1418;border-color:var(--red)}\n"
  ".seg{display:flex;gap:0}\n"
  ".seg button{border-radius:0}\n"
  ".seg button:first-child{border-radius:7px 0 0 7px}\n"
  ".seg button:last-child{border-radius:0 7px 7px 0;border-left:0}\n"
  ".console{max-width:1100px;margin:0 auto 28px;padding:0 20px}\n"
  ".console .box{background:#05080c;border:1px solid var(--line);border-radius:10px;height:200px;\n"
  "  overflow:auto;padding:12px 14px}\n"
  ".console h2{margin:0 0 8px;font:600 11px/1 ui-monospace,Menlo,Consolas,monospace;\n"
  "  letter-spacing:.2em;color:var(--muted)}\n"
  "#log div{font:12.5px/1.5 ui-monospace,Menlo,Consolas,monospace;white-space:pre-wrap;word-break:break-word}\n"
  "#log .out{color:var(--blue)}\n"
  "#log .ok{color:var(--ok)}\n"
  "#log .err{color:var(--red)}\n"
  "#log .sys{color:var(--muted)}\n"
  ".marquee{max-width:1100px;margin:14px auto 0;background:#0c0704;border:1px solid #2c1e08;\n"
  "  border-radius:10px;overflow:hidden;display:flex;align-items:center;\n"
  "  box-shadow:inset 0 0 34px rgba(0,0,0,.6)}\n"
  ".mqtrack{display:inline-block;white-space:nowrap;will-change:transform;\n"
  "  font-weight:700;color:#ffce6b;letter-spacing:.02em;\n"
  "  font-family:system-ui,-apple-system,Segoe UI,Roboto,sans-serif}\n"
  "@media (prefers-reduced-motion:reduce){.scan i{animation:none;margin-left:33%}}\n"
  "</style></head><body>\n"
  "<header>\n"
  "  <div class=brand>\n"
  "    <h1>PI400&nbsp;CONSOLE</h1>\n"
  "    <span class=sub mono>BARE-METAL · FreeRTOS · GENET</span>\n"
  "  </div>\n"
  "  <div class=scan><i></i></div>\n"
  "  <div class='status mono'>command bridge online — every action runs through the same parser as the serial console&nbsp; · &nbsp;host <b id=host>…</b></div>\n"
  "</header>\n"
  "\n"
  "<div class=marquee><div class=mqtrack id=mqtrack></div></div>\n"
  "\n"
  "<main>\n"
  "  <section class=card>\n"
  "    <h2>CLOCK</h2>\n"
  "    <div class=row><label>set</label>\n"
  "      <input id=t placeholder='HH:MM:SS'>\n"
  "      <button class=go onclick=\"sendv('t','set')\">set</button></div>\n"
  "    <div class=row tight><button onclick=\"send('time')\">show time</button></div>\n"
  "  </section>\n"
  "\n"
  "  <section class=card>\n"
  "    <h2>MARQUEE</h2>\n"
  "    <div class=row><input id=m maxlength=200 placeholder='msg (shown uppercase)'>\n"
  "      <button class=go onclick=\"sendv('m','msg')\">send</button></div>\n"
  "    <div class=row><input id=nm maxlength=200 placeholder='netmsg (original case)'>\n"
  "      <button class=go onclick=\"sendv('nm','netmsg')\">send</button></div>\n"
  "  </section>\n"
  "\n"
  "  <section class=card>\n"
  "    <h2>QR CODE</h2>\n"
  "    <div class=row><input id=qf placeholder='full-screen QR text/url'>\n"
  "      <button class=go onclick=\"sendv('qf','qrfull')\">show</button></div>\n"
  "    <div class=row><input id=qs placeholder='small QR text/url'>\n"
  "      <button class=go onclick=\"sendv('qs','qrsmall')\">show</button></div>\n"
  "    <div class=row tight><button onclick=\"send('qr off')\">clear QR</button></div>\n"
  "  </section>\n"
  "\n"
  "  <section class=card>\n"
  "    <h2>LED SWEEP</h2>\n"
  "    <div class=row><div class=seg>\n"
  "      <button class=go onclick=\"send('run')\">run</button>\n"
  "      <button class=stop onclick=\"send('stop')\">stop</button></div></div>\n"
  "    <div class=row><label>speed</label>\n"
  "      <input id=sp placeholder='20–1000 ms'>\n"
  "      <button onclick=\"sendv('sp','speed')\">set</button></div>\n"
  "    <div class=row><label>leds</label>\n"
  "      <input id=ld placeholder='mask 0–255'>\n"
  "      <button onclick=\"sendv('ld','leds')\">set</button></div>\n"
  "  </section>\n"
  "\n"
  "  <section class=card>\n"
  "    <h2>SERVO</h2>\n"
  "    <div class=row><div class=seg>\n"
  "      <button class=go onclick=\"send('servo run')\">run</button>\n"
  "      <button class=stop onclick=\"send('servo stop')\">stop</button></div></div>\n"
  "    <div class=row><label>angle</label>\n"
  "      <input id=sva placeholder='0–180°'>\n"
  "      <button onclick=\"sendv('sva','servo')\">go</button></div>\n"
  "    <div class=row><label>speed</label>\n"
  "      <input id=svs placeholder='1–30'>\n"
  "      <button onclick=\"sendv('svs','servo speed')\">set</button></div>\n"
  "    <div class=row tight><label>dir</label><div class=seg>\n"
  "      <button onclick=\"send('servo dir 0')\">0</button>\n"
  "      <button onclick=\"send('servo dir 1')\">1</button></div></div>\n"
  "  </section>\n"
  "\n"
  "  <section class=card>\n"
  "    <h2>STEPPER</h2>\n"
  "    <div class=row><div class=seg>\n"
  "      <button class=go onclick=\"send('step run')\">run</button>\n"
  "      <button class=stop onclick=\"send('step stop')\">stop</button></div></div>\n"
  "    <div class=row><label>speed</label>\n"
  "      <input id=sts placeholder='1–1000 sps'>\n"
  "      <button onclick=\"sendv('sts','step speed')\">set</button></div>\n"
  "    <div class=row tight><label>dir</label><div class=seg>\n"
  "      <button onclick=\"send('step dir 0')\">0</button>\n"
  "      <button onclick=\"send('step dir 1')\">1</button></div></div>\n"
  "  </section>\n"
  "\n"
  "  <section class=card>\n"
  "    <h2>MAX7219</h2>\n"
  "    <div class=row><label>bright</label>\n"
  "      <input id=br placeholder='0–15'>\n"
  "      <button onclick=\"sendv('br','bright')\">set</button></div>\n"
  "  </section>\n"
  "\n"
  "  <section class=card>\n"
  "    <h2>PRAYER</h2>\n"
  "    <div class=row><input id=pn placeholder='name' style=flex:0.8>\n"
  "      <input id=pt placeholder='HH:MM' style=flex:0.7>\n"
  "      <button class=go onclick=\"sendPray()\">show</button></div>\n"
  "    <div class=row tight><button onclick=\"send('prayerclr')\">clear prayer</button></div>\n"
  "  </section>\n"
  "\n"
  "  <section class=card>\n"
  "    <h2>INSTRUMENT</h2>\n"
  "    <div class=row><label>LA</label>\n"
  "      <input id=la placeholder='logic-analyzer args'>\n"
  "      <button onclick=\"sendv('la','la')\">view</button></div>\n"
  "    <div class=row><label>scope</label>\n"
  "      <input id=sc placeholder='oscilloscope args'>\n"
  "      <button onclick=\"sendv('sc','sc')\">view</button></div>\n"
  "  </section>\n"
  "\n"
  "  <section class=card>\n"
  "    <h2>SYSTEM</h2>\n"
  "    <div class=row>\n"
  "      <button onclick=\"send('list')\">list</button>\n"
  "      <button onclick=\"send('help')\">help</button></div>\n"
  "    <div class=row tight><input id=raw placeholder='raw command…'>\n"
  "      <button class=go onclick=\"sendv('raw','')\">run</button></div>\n"
  "  </section>\n"
  "</main>\n"
  "\n"
  "<div class=console>\n"
  "  <h2 class=mono>CONSOLE</h2>\n"
  "  <div class=box id=log></div>\n"
  "</div>\n"
  "\n"
  "<script>\n"
  "const $=s=>document.getElementById(s);\n"
  "function logln(s,cls){const l=$('log');const d=document.createElement('div');\n"
  "  if(cls)d.className=cls;d.textContent=s;l.appendChild(d);l.scrollTop=l.scrollHeight;}\n"
  "async function send(cmd){\n"
  "  cmd=(cmd||'').trim(); if(!cmd){logln('enter a value first','err');return;}\n"
  "  logln('› '+cmd,'out');\n"
  "  try{\n"
  "    const r=await fetch('/cmd?c='+encodeURIComponent(cmd));\n"
  "    const t=(await r.text()).trim();\n"
  "    logln(t||('HTTP '+r.status), r.ok?'ok':'err');\n"
  "  }catch(e){ logln('unreachable — '+e,'err'); }\n"
  "}\n"
  "function sendv(id,prefix){\n"
  "  const v=$(id).value.trim();\n"
  "  if(!v){logln('enter a value for '+(prefix||'command')+' first','err');return;}\n"
  "  send(prefix? prefix+' '+v : v);\n"
  "}\n"
  "function sendPray(){\n"
  "  const n=$('pn').value.trim(), t=$('pt').value.trim();\n"
  "  if(!n||!t){logln('prayer needs a name and HH:MM','err');return;}\n"
  "  send('pray '+n+' '+t);\n"
  "}\n"
  "addEventListener('keydown',e=>{\n"
  "  if(e.key!=='Enter')return;\n"
  "  const a=document.activeElement; if(a&&a.tagName==='INPUT'){\n"
  "    const b=a.closest('.row').querySelector('button'); if(b)b.click();\n"
  "  }\n"
  "});\n"
  "$('host').textContent=location.host||'pi400';\n"
  "logln('console ready — pick a command above','sys');\n"
  "\n"
  "/* ---- scrolling ticker, mirroring the HDMI marquee (g_marquee) ---- */\n"
  "(function(){\n"
  "  const wrap=document.querySelector('.marquee'), track=document.getElementById('mqtrack');\n"
  "  let cur='', x=0, W=0, tW=0;\n"
  "  const SPEED=1.4;                                   // px/frame (~84 px/s)\n"
  "  function measure(){ W=wrap.clientWidth; tW=track.scrollWidth; }\n"
  "  function size(){\n"
  "    const card=document.querySelector('.card'), ch=card?card.offsetHeight:170;\n"
  "    const bar=Math.round(ch/3);                      // 1/3 of a card\n"
  "    wrap.style.height=bar+'px';\n"
  "    track.style.fontSize=Math.round(bar*0.6)+'px';\n"
  "    measure();\n"
  "    if(x>W || x< -tW) x=W;\n"
  "  }\n"
  "  function set(t){ cur=t; track.textContent=t; measure(); x=W; }   // start at right edge\n"
  "  function frame(){\n"
  "    x-=SPEED;\n"
  "    if(x < -tW) x=W;                                 // fully off-screen -> repeat from right\n"
  "    track.style.transform='translateX('+x+'px)';\n"
  "    requestAnimationFrame(frame);\n"
  "  }\n"
  "  async function poll(){\n"
  "    try{const r=await fetch('/marquee');if(r.ok){const t=(await r.text()).trim();\n"
  "      if(t&&t!==cur)set(t);}}catch(e){}\n"
  "  }\n"
  "  set('KNIGHT RIDER CLOCK'); requestAnimationFrame(frame);\n"
  "  poll(); setInterval(poll,1500); addEventListener('resize',size);\n"
  "})();\n"
  "</script>\n"
  "</body></html>\n"
  "";

/* HTTP -> parse_line() bridge. A request to /cmd carries a command either as
 * the query var ?c=...  (GET, easy from a browser) or as the request body
 * (POST, easy from curl). The command text is exactly what you'd type on the
 * UART console; parse_line() enqueues it on g_cmdq just like the UART task. */
static void ev_handler(struct mg_connection *c, int ev, void *ev_data) {
    if (ev != MG_EV_HTTP_MSG) return;
    struct mg_http_message *hm = (struct mg_http_message *) ev_data;

#if WEB_AUTH
    {
        char user[48], pass[48];
        mg_http_creds(hm, user, sizeof(user), pass, sizeof(pass));
        if (strcmp(user, WEB_USER) != 0 || strcmp(pass, WEB_PASS) != 0) {
            mg_http_reply(c, 401,
                "WWW-Authenticate: Basic realm=\"PI400 Console\"\r\n"
                "Content-Type: text/plain\r\n",
                "authentication required\r\n");
            return;
        }
    }
#endif

    if (mg_match(hm->uri, mg_str("/marquee"), NULL)) {
        mg_http_reply(c, 200, "Content-Type: text/plain\r\n", "%s", g_marquee);
        return;
    }

    if (mg_match(hm->uri, mg_str("/cmd"), NULL)) {
        char cmd[256];
        int n = mg_http_get_var(&hm->query, "c", cmd, sizeof(cmd));   /* ?c=... */
        if (n <= 0 && hm->body.len > 0) {                             /* or POST body */
            size_t len = hm->body.len < sizeof(cmd) - 1 ? hm->body.len : sizeof(cmd) - 1;
            for (size_t i = 0; i < len; i++) cmd[i] = hm->body.buf[i];
            cmd[len] = 0; n = (int) len;
        }
        if (n > 0) {
            while (n > 0 && (cmd[n-1] == '\r' || cmd[n-1] == '\n' || cmd[n-1] == ' '))
                cmd[--n] = 0;                                         /* trim trailing ws */
            parse_line(cmd);
            mg_http_reply(c, 200, "Content-Type: text/plain\r\n", "OK: %s\n", cmd);
        } else {
            mg_http_reply(c, 400, "Content-Type: text/plain\r\n", "no command\n");
        }
    } else {
        mg_http_reply(c, 200, "Content-Type: text/html\r\n", "%s", DASH);
    }
}

#if ENABLE_NTP
static struct mg_connection *s_sntp;
static int  s_ntp_synced;
static char s_dnsurl[28];   /* "udp://A.B.C.D:53"  from DHCP (or fallback)    */
static char s_ntpurl[28];   /* "udp://A.B.C.D:123" from DHCP SNTP, if offered  */
static char s_gwurl[28];    /* "udp://<gateway>:123" fallback NTP target       */

/* Build "udp://A.B.C.D:port" from a network-order IPv4 address. */
static void ip4_url(char *dst, uint32_t ip_net, int port) {
    const uint8_t *o = (const uint8_t *) &ip_net;
    int n = 0; const char *p = "udp://";
    while (*p) dst[n++] = *p++;
    for (int i = 0; i < 4; i++) {
        uint8_t v = o[i];
        if (v >= 100) dst[n++] = (char)('0' + v / 100);
        if (v >= 10)  dst[n++] = (char)('0' + (v / 10) % 10);
        dst[n++] = (char)('0' + v % 10);
        dst[n++] = (i < 3) ? '.' : ':';
    }
    if (port >= 100) dst[n++] = (char)('0' + port / 100);
    if (port >= 10)  dst[n++] = (char)('0' + (port / 10) % 10);
    dst[n++] = (char)('0' + port % 10);
    dst[n] = 0;
}

static void setup_dns(void) {
    if (s_dnsurl[0]) return;                        /* already set from DHCP   */
    if (DNS_URL[0]) { s_mgr.dns4.url = DNS_URL; uart_puts("NTP: DNS " DNS_URL "\r\n"); return; }
    ip4_url(s_dnsurl, s_mif.gw, 53);                /* last resort: gateway    */
    s_mgr.dns4.url = s_dnsurl;
    uart_puts("NTP: DNS (gw) "); uart_puts(s_dnsurl); uart_puts("\r\n");
}

static void sntp_fn(struct mg_connection *c, int ev, void *ev_data) {
    (void) c;
    if (ev == MG_EV_SNTP_TIME) {
        uint64_t ms   = *(uint64_t *) ev_data;
        uint64_t secs = ms / 1000u + (uint64_t)(TZ_OFFSET_MIN) * 60u;
        uint32_t tod  = (uint32_t)(secs % 86400u);
        int hh = (int)(tod / 3600), mm = (int)((tod % 3600) / 60), ss = (int)(tod % 60);
        char line[16]; int n = 0;
        line[n++]='s'; line[n++]='e'; line[n++]='t'; line[n++]=' ';
        line[n++]=(char)('0'+hh/10); line[n++]=(char)('0'+hh%10); line[n++]=':';
        line[n++]=(char)('0'+mm/10); line[n++]=(char)('0'+mm%10); line[n++]=':';
        line[n++]=(char)('0'+ss/10); line[n++]=(char)('0'+ss%10); line[n]=0;
        parse_line(line);
        s_ntp_synced = 1;
        uart_puts("NTP: clock set "); uart_puts(line + 4); uart_puts("\r\n");
    } else if (ev == MG_EV_ERROR) {
        uart_puts("NTP: error "); uart_puts(ev_data ? (char *) ev_data : "?"); uart_puts("\r\n");
        s_sntp = NULL;
    } else if (ev == MG_EV_CLOSE) {
        s_sntp = NULL;
    }
}

static void sntp_timer(void *arg) {
    static uint32_t tick, attempts;
    struct mg_mgr *mgr = (struct mg_mgr *) arg;
    if (s_ntp_synced && (++tick % 30u) != 0u) return;     /* fast retry, then re-sync */
    if (s_sntp != NULL) { s_sntp->is_closing = 1; s_sntp = NULL; }  /* drop a silent attempt */
    const char *url;
    if (s_ntpurl[0]) {
        url = s_ntpurl;                                   /* DHCP-advertised NTP (IP) */
    } else if (attempts >= 4u && s_mif.gw) {
        ip4_url(s_gwurl, s_mif.gw, 123);                  /* fallback: the gateway/router */
        url = s_gwurl;
        if (attempts == 4u) { uart_puts("NTP: falling back to gateway "); uart_puts(s_gwurl); uart_puts("\r\n"); }
    } else {
        url = NTP_URL;
    }
    attempts++;
    s_sntp = mg_sntp_connect(mgr, url, sntp_fn, NULL);
    if (s_sntp != NULL) mg_sntp_request(s_sntp);
}

/* Interface event handler: capture DHCP-assigned DNS/SNTP, and kick off the
 * first time-sync once the stack actually reaches the READY state. */
static void mif_fn(struct mg_tcpip_if *ifp, int ev, void *ev_data) {
    (void) ifp;
    if (ev == MG_TCPIP_EV_DHCP_DNS) {
        ip4_url(s_dnsurl, *(uint32_t *) ev_data, 53);
        s_mgr.dns4.url = s_dnsurl;
        uart_puts("NET: DHCP DNS  "); uart_puts(s_dnsurl); uart_puts("\r\n");
    } else if (ev == MG_TCPIP_EV_DHCP_SNTP) {
        ip4_url(s_ntpurl, *(uint32_t *) ev_data, 123);
        uart_puts("NET: DHCP SNTP "); uart_puts(s_ntpurl); uart_puts("\r\n");
    } else if (ev == MG_TCPIP_EV_STATE_CHANGE) {
        if (*(uint8_t *) ev_data == MG_TCPIP_STATE_READY) {
            setup_dns();
            sntp_timer(&s_mgr);                           /* network is truly up now */
        }
    }
}
#endif

#if ENABLE_WS
static struct mg_connection *s_ws;

static int wsapp(char *dst, int pos, int cap, const char *src) {
    while (src && *src && pos < cap - 1) dst[pos++] = *src++;
    return pos;
}

/* Turn a relay frame like
 *   {"message":{"host":"192.168.1.1","status":"down","since":"jun/26/2026 14:57:00"}}
 * into  HOST 192.168.1.1 DOWN SINCE JUN/26/2026 14:57:00  (uppercase, so it's
 * visually distinct from the mixed-case netmsg the ESP32 sends). Non-matching
 * frames are forwarded as-is. */
static void ws_fn(struct mg_connection *c, int ev, void *ev_data) {
    (void) c;
    if (ev == MG_EV_WS_OPEN) {
        uart_puts("WS: connected\r\n");
    } else if (ev == MG_EV_WS_MSG) {
        struct mg_ws_message *wm = (struct mg_ws_message *) ev_data;
        char line[256]; int n = 0;
        n = wsapp(line, n, (int) sizeof(line), "netmsg ");

        char *host   = mg_json_get_str(wm->data, "$.message.host");
        char *status = mg_json_get_str(wm->data, "$.message.status");
        char *since  = mg_json_get_str(wm->data, "$.message.since");

        if (host && status && since) {
            n = wsapp(line, n, (int) sizeof(line), "HOST ");
            n = wsapp(line, n, (int) sizeof(line), host);
            if (n < (int) sizeof(line) - 1) line[n++] = ' ';
            n = wsapp(line, n, (int) sizeof(line), status);
            n = wsapp(line, n, (int) sizeof(line), " SINCE ");
            n = wsapp(line, n, (int) sizeof(line), since);
        } else {
            for (size_t i = 0; i < wm->data.len && n < (int) sizeof(line) - 1; i++)
                line[n++] = wm->data.buf[i];
        }
        line[n] = 0;
        for (int i = 7; line[i]; i++)                 /* uppercase the message body */
            if (line[i] >= 'a' && line[i] <= 'z') line[i] = (char)(line[i] - 32);
        parse_line(line);                             /* netmsg path (gated) */

        free(host); free(status); free(since);
    } else if (ev == MG_EV_CLOSE || ev == MG_EV_ERROR) {
        s_ws = NULL;
    }
}
static void ws_timer(void *arg) {
    struct mg_mgr *mgr = (struct mg_mgr *) arg;
    if (s_ws == NULL) s_ws = mg_ws_connect(mgr, WS_URL, ws_fn, NULL, NULL);
}
#endif

static void net_task(void *arg) {
    (void)arg;
#if defined(__aarch64__)
    /* Pi 3/400 (AArch64): Mongoose is built hard-float, so enable FP/SIMD
     * access in this task's context. The rest of the kernel is integer-only
     * and only this task ever touches FP. Pi 1 (ARMv6) is soft-float -- FP
     * goes through libgcc, so no register enable is needed (and cpacr_el1 /
     * isb don't exist on ARM1176). */
    uint64_t cpacr;
    __asm__ volatile("mrs %0, cpacr_el1" : "=r"(cpacr));
    cpacr |= (3ULL << 20);
    __asm__ volatile("msr cpacr_el1, %0; isb" :: "r"(cpacr));
#endif

    mg_mgr_init(&s_mgr);
    memset(&s_mif, 0, sizeof(s_mif));
    genet_get_mac(s_mif.mac);
    s_mif.driver = &s_genet_driver;
    s_mif.driver_data = NULL;
    s_mif.ip = 0;                          /* 0 => DHCP */
#if ENABLE_NTP
    s_mif.enable_req_dns  = true;          /* ask DHCP for a DNS server   */
    s_mif.enable_req_sntp = true;          /* ask DHCP for an SNTP server  */
    s_mif.fn = mif_fn;                     /* capture them as they arrive  */
#endif
    mg_tcpip_init(&s_mgr, &s_mif);
    mg_http_listen(&s_mgr, "http://0.0.0.0:80", ev_handler, NULL);
#if ENABLE_NTP
    mg_timer_add(&s_mgr, 15000, MG_TIMER_REPEAT, sntp_timer, &s_mgr);     /* retry/re-sync */
#endif
#if ENABLE_WS
    mg_timer_add(&s_mgr, 3000, MG_TIMER_REPEAT | MG_TIMER_RUN_NOW, ws_timer, &s_mgr);  /* connect/keep-alive */
#endif
    uart_puts("NET: stack up, HTTP on :80, waiting for DHCP...\r\n");

    uint32_t last = 0xFFFFFFFFu;
#if STATIC_FALLBACK
    uint64_t t_up = mg_millis();
    int fb_decided = 0;
#endif
    for (;;) {
        mg_mgr_poll(&s_mgr, 1);
#if STATIC_FALLBACK
        if (!fb_decided) {
            if (s_mif.ip) {
                fb_decided = 1;                   /* DHCP got a lease; no fallback */
            } else if (mg_millis() - t_up > STATIC_DHCP_WAIT_MS) {
                s_mif.enable_dhcp_client = false; /* stop DHCP; stack uses static IP */
                s_mif.ip   = STATIC_IP;           /* UP && !dhcp && ip -> IP -> READY */
                s_mif.mask = STATIC_MASK;
                s_mif.gw   = STATIC_GW;
                uart_puts("NET: no DHCP -> static ");
                put_ip(s_mif.ip); uart_puts(" gw "); put_ip(s_mif.gw); uart_puts("\r\n");
                fb_decided = 1;
            }
        }
#endif
        if (s_mif.ip != last) {
            last = s_mif.ip;
            if (s_mif.ip) {
                uart_puts("NET: ready  http://"); put_ip(s_mif.ip); uart_puts("/\r\n");
            }
        }
        vTaskDelay(1);
    }
}

/* Called from main() to start networking. */
void net_start(void) {
#if defined(__aarch64__)
    xTaskCreate(net_task, "net", 4096, NULL, 3, NULL);
#else
    /* Pi 1 (ARMv6) is soft-float: every double op is a nested __aeabi_* call,
     * so Mongoose's DHCP/SNTP/HTTP parsing needs a deeper stack than hard-float
     * Pi 3. 4096 words overflows and silently corrupts the neighbouring task. */
    xTaskCreate(net_task, "net", 8192, NULL, 3, NULL);
#endif
}