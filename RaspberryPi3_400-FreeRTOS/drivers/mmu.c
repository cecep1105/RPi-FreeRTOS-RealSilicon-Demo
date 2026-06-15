#include <stdint.h>

/* Identity-map the low 2GB so unaligned accesses are legal on real silicon.
 * With the MMU OFF, all RAM is Device-nGnRnE, which faults on any unaligned
 * access (e.g. a 64-bit LDR from a 4-byte-aligned literal pool). Mapping RAM
 * as Normal memory makes such accesses legal (SCTLR.A=0).
 *
 *   0x00000000 - 0x3EFFFFFF : Normal  (RAM)
 *   0x3F000000 - 0x3FFFFFFF : Device  (BCM2837 peripherals)
 *   0x40000000 - 0x7FFFFFFF : Device  (ARM local controller, timer IRQ routing)
 *
 * RAM is left Non-Cacheable for now (no cache-maintenance burden, no DMA/
 * framebuffer coherency concerns); the alignment fix does not need caching.
 */

#define PERIPH_BASE  0x3F000000ULL

__attribute__((aligned(4096))) static uint64_t l1[512];
__attribute__((aligned(4096))) static uint64_t l2[512];

/* block/page lower attributes */
#define AF        (1ULL << 10)          /* Access Flag                     */
#define VALID     (1ULL << 0)           /* bit0=1, bit1=0 => block at L1/L2 */
#define TABLE     (3ULL << 0)           /* bits[1:0]=11   => table at L1    */
#define ATTRIDX(n) ((uint64_t)(n) << 2) /* MAIR index                      */
/* MAIR: attr0 = Device-nGnRnE (0x00); attr1 = Normal Non-Cacheable (0x44) */
#define MAIR_VAL  ((0x00ULL << 0) | (0x44ULL << 8))
#define IDX_DEVICE 0
#define IDX_NORMAL 1

void mmu_init(void)
{
    for (int i = 0; i < 512; i++) {
        uint64_t pa = (uint64_t)i << 21;           /* 2 MB blocks */
        if (pa < PERIPH_BASE)
            l2[i] = pa | AF | ATTRIDX(IDX_NORMAL) | VALID;   /* RAM: Normal */
        else
            l2[i] = pa | AF | ATTRIDX(IDX_DEVICE) | VALID;   /* peripherals */
    }
    l1[0] = (uint64_t)l2 | TABLE;                  /* 0x0-0x3FFFFFFF via L2  */
    l1[1] = 0x40000000ULL | AF | ATTRIDX(IDX_DEVICE) | VALID; /* 1GB Device  */
    for (int i = 2; i < 512; i++) l1[i] = 0;

    __asm__ volatile("msr mair_el1, %0" :: "r"(MAIR_VAL));

    /* TCR: T0SZ=25 (39-bit VA -> start at level 1), 4KB granule, walks NC,
       non-shareable, TTBR1 disabled, IPS=32-bit. */
    uint64_t tcr = (25ULL << 0)   /* T0SZ            */
                 | (0ULL  << 8)   /* IRGN0 = NC      */
                 | (0ULL  << 10)  /* ORGN0 = NC      */
                 | (0ULL  << 12)  /* SH0   = none    */
                 | (0ULL  << 14)  /* TG0   = 4KB     */
                 | (1ULL  << 23)  /* EPD1  = 1        */
                 | (0ULL  << 32); /* IPS   = 32-bit  */
    __asm__ volatile("msr tcr_el1, %0" :: "r"(tcr));

    __asm__ volatile("msr ttbr0_el1, %0" :: "r"((uint64_t)l1));
    __asm__ volatile("dsb sy; isb");
    __asm__ volatile("tlbi vmalle1; dsb sy; isb");

    uint64_t sctlr;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |=  (1ULL << 0);   /* M = 1  : MMU on                       */
    sctlr &= ~(1ULL << 1);   /* A = 0  : allow unaligned (Normal mem) */
    sctlr &= ~(1ULL << 2);   /* C = 0  : data cache off (RAM is NC)   */
    sctlr &= ~(1ULL << 12);  /* I = 0  : instruction cache off        */
    __asm__ volatile("msr sctlr_el1, %0; isb" :: "r"(sctlr));
}
