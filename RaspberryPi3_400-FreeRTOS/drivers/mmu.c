#include <stdint.h>
#include "peripherals.h"          /* PERIPHERAL_BASE: 0x3F000000 (Pi3) | 0xFE000000 (Pi4/400) */

/* Identity-map the low address space so unaligned accesses are legal on real
 * silicon, and so all peripherals each target needs are reachable.
 *
 * With the MMU OFF, all RAM is Device-nGnRnE, which faults on any unaligned
 * access (e.g. a 64-bit LDR from a 4-byte-aligned literal pool). Mapping RAM
 * as Normal memory makes such accesses legal (SCTLR.A=0).
 *
 * Pi 2/3 (BCM2836/7):
 *   0x00000000 - 0x3EFFFFFF : Normal  (RAM)
 *   0x3F000000 - 0x3FFFFFFF : Device  (peripherals)
 *   0x40000000 - 0x7FFFFFFF : Device  (ARM local controller, timer IRQ routing)
 *
 * Pi 4/400 (BCM2711, low-peripheral mode):
 *   0x00000000 - 0xBFFFFFFF : Normal  (RAM)
 *   0xC0000000 - 0xFFFFFFFF : Device  (GENET @0xFD580000, main peripherals
 *                                      @0xFE000000, GIC-400 @0xFF840000)
 *
 * RAM is left Non-Cacheable and the data cache is OFF: no cache-maintenance
 * burden and, importantly, GENET (a DMA bus-master) and the CPU see the same
 * memory with no cache between them, so the descriptor rings need no
 * clean/invalidate -- only ordering barriers (dsb).
 */

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

#define BLK_NORMAL(pa) ((uint64_t)(pa) | AF | ATTRIDX(IDX_NORMAL) | VALID)
#define BLK_DEVICE(pa) ((uint64_t)(pa) | AF | ATTRIDX(IDX_DEVICE) | VALID)

void mmu_init(void)
{
#if (PERIPHERAL_BASE == 0xFE000000UL)
    /* ---------------- BCM2711 (Pi 4 / Pi 400) ---------------- */
    /* Low 1GB: all Normal RAM (no device hole here on BCM2711). */
    for (int i = 0; i < 512; i++)
        l2[i] = BLK_NORMAL((uint64_t)i << 21);          /* 2 MB blocks */

    l1[0] = (uint64_t)l2 | TABLE;                       /* 0x00000000-0x3FFFFFFF RAM   */
    l1[1] = BLK_NORMAL(0x40000000ULL);                  /* 0x40000000-0x7FFFFFFF RAM   */
    l1[2] = BLK_NORMAL(0x80000000ULL);                  /* 0x80000000-0xBFFFFFFF RAM   */
    l1[3] = BLK_DEVICE(0xC0000000ULL);                  /* 0xC0000000-0xFFFFFFFF Device:
                                                           GENET 0xFD580000, periph
                                                           0xFE000000, GIC 0xFF840000  */
    for (int i = 4; i < 512; i++) l1[i] = 0;
#else
    /* ---------------- BCM2836 / BCM2837 (Pi 2 / Pi 3) -------- */
    for (int i = 0; i < 512; i++) {
        uint64_t pa = (uint64_t)i << 21;                /* 2 MB blocks */
        if (pa < 0x3F000000ULL)
            l2[i] = BLK_NORMAL(pa);                     /* RAM: Normal        */
        else
            l2[i] = BLK_DEVICE(pa);                     /* peripherals: Device */
    }
    l1[0] = (uint64_t)l2 | TABLE;                       /* 0x0-0x3FFFFFFF via L2 */
    l1[1] = BLK_DEVICE(0x40000000ULL);                  /* 1GB Device (local ctrl) */
    for (int i = 2; i < 512; i++) l1[i] = 0;
#endif

    __asm__ volatile("msr mair_el1, %0" :: "r"(MAIR_VAL));

    /* TCR: T0SZ=25 (39-bit VA -> start at level 1), 4KB granule, walks NC,
       non-shareable, TTBR1 disabled, IPS=32-bit. */
    uint64_t tcr = (25ULL << 0)   /* T0SZ            */
                 | (0ULL  << 8)   /* IRGN0 = NC      */
                 | (0ULL  << 10)  /* ORGN0 = NC      */
                 | (0ULL  << 12)  /* SH0   = none    */
                 | (0ULL  << 14)  /* TG0   = 4KB     */
                 | (1ULL  << 23)  /* EPD1  = 1       */
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

    /* Allow FP/SIMD at EL0/EL1 (CPACR_EL1.FPEN = 0b11). The kernel is
     * otherwise integer-only, but the Mongoose TCP/IP stack uses double in a
     * few formatting helpers. Only the network task touches FP, so nothing
     * clobbers these registers across a context switch. */
    uint64_t cpacr;
    __asm__ volatile("mrs %0, cpacr_el1" : "=r"(cpacr));
    cpacr |= (3ULL << 20);
    __asm__ volatile("msr cpacr_el1, %0; isb" :: "r"(cpacr));
}