/* aligned_mem.c — alignment-safe memcpy/memmove for Raspberry Pi 1 (ARMv6, MMU off)
 *
 * WHY THIS EXISTS
 * ---------------
 * newlib's ARMv6 memcpy copies 32-bit words and assumes the CPU performs
 * unaligned word loads in hardware. On the Pi 1 (ARM1176, ARMv6) running
 * bare-metal with the MMU disabled, memory is strongly-ordered and an
 * unaligned LDR falls back to the legacy "rotate" behaviour: it reads the
 * word-aligned container and rotates right by 8*(addr & 3) bits instead of
 * loading the requested bytes. A 4-byte copy from a 2-byte-aligned source
 * (e.g. Mongoose's DHCP xid = memcpy(&xid, ifp->mac + 2, 4), where ifp->mac
 * is 8-byte aligned) therefore comes out as mac[2] mac[3] mac[0] mac[1]
 * instead of mac[2..5] — silently corrupting the xid and breaking DHCP.
 *
 * These strong definitions override newlib's so EVERY memcpy/memmove in the
 * image is alignment-safe. The fast 32-bit path runs only when BOTH pointers
 * are word-aligned; any unaligned operand falls back to a byte copy, which is
 * correct regardless of the CPU's unaligned-access support.
 *
 * Pi 3 / Pi 400 (AArch64) build with -nostdlib and their own kstring.c shims,
 * so this file is for the Pi 1 (newlib) build only — drop it in drivers/.
 */

#include <stddef.h>
#include <stdint.h>

void *memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *) dst;
    const uint8_t *s = (const uint8_t *) src;

    /* Fast path: both word-aligned -> safe 32-bit copies. */
    if ((((uintptr_t) d | (uintptr_t) s) & 3u) == 0u) {
        while (n >= 4u) {
            *(uint32_t *) d = *(const uint32_t *) s;
            d += 4; s += 4; n -= 4u;
        }
    }
    /* Tail / unaligned: byte copy (alignment-independent). */
    while (n--) *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *) dst;
    const uint8_t *s = (const uint8_t *) src;

    if (d == s || n == 0u) return dst;

    if (d < s) {                 /* non-overlapping or forward-safe */
        if ((((uintptr_t) d | (uintptr_t) s) & 3u) == 0u) {
            while (n >= 4u) {
                *(uint32_t *) d = *(const uint32_t *) s;
                d += 4; s += 4; n -= 4u;
            }
        }
        while (n--) *d++ = *s++;
    } else {                     /* dst above src -> copy backwards, byte-wise */
        d += n; s += n;
        while (n--) *--d = *--s;
    }
    return dst;
}