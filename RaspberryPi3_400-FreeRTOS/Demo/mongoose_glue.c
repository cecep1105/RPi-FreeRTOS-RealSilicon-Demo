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

/* ----- route Mongoose log output to UART ----- */
static void log_putchar(char c, void *param) { (void)param; uart_putc(c); }

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
static void net_task(void *arg) {
    (void)arg;
    struct mg_mgr mgr;

    mg_log_set_fn(log_putchar, NULL);
    mg_log_set(MG_LL_INFO);
    mg_mgr_init(&mgr);

    static struct mg_tcpip_if mif;
    memset(&mif, 0, sizeof(mif));
    genet_get_mac(mif.mac);
    mif.driver = &s_genet_driver;
    mif.driver_data = NULL;
    mif.ip = 0;                /* 0 => DHCP */

    mg_tcpip_init(&mgr, &mif);
    MG_INFO(("net_task up, MAC %02x:%02x:%02x:%02x:%02x:%02x, waiting for DHCP...",
             mif.mac[0], mif.mac[1], mif.mac[2], mif.mac[3], mif.mac[4], mif.mac[5]));

    for (;;) {
        mg_mgr_poll(&mgr, 1);
        vTaskDelay(1);
    }
}

/* Called from main() to start networking. */
void net_start(void) {
    xTaskCreate(net_task, "net", 4096, NULL, 3, NULL);
}