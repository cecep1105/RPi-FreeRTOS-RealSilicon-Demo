#include <stddef.h>
#include <stdint.h>
/* Freestanding mem* not already provided by the FreeRTOS port (port.c has memcpy). */
void *memset(void *s, int c, size_t n){ uint8_t *p=s; while(n--) *p++=(uint8_t)c; return s; }

/* memmove + abs: needed by drivers/qrcodegen.c on the -nostdlib build.
 * (memcpy lives in port.c, memset above; the upstream encoder also calls
 *  memmove for its bit-buffer shifts and abs() for module geometry.) */
void *memmove(void *dst, const void *src, size_t n){
    uint8_t *d = dst; const uint8_t *s = src;
    if(d == s || n == 0) return dst;
    if(d < s){ while(n--) *d++ = *s++; }
    else { d += n; s += n; while(n--) *--d = *--s; }
    return dst;
}
int abs(int v){ return v < 0 ? -v : v; }
