#include "peripherals.h"
#include "irqctrl.h"

#define R32(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define R8(a)  (*(volatile uint8_t  *)(uintptr_t)(a))

#if (PERIPHERAL_BASE == 0xFE000000UL)
/* ===================== BCM2711 (Pi 4 / Pi 400): GIC-400 ================= */
/* GIC-400 lives at a fixed address, independent of the 0xFE.. peripheral base. */
#define GICD 0xFF841000UL        /* Distributor   */
#define GICC 0xFF842000UL        /* CPU interface */
#define CNTV_PPI 27              /* virtual-timer private peripheral int    */

void kr_irqctrl_init(void)
{
    /* CPU interface */
    R32(GICC + 0x04) = 0xFF;             /* PMR: allow all priorities       */
    R32(GICC + 0x00) = 0x01;             /* CTLR: enable signalling         */
    /* Distributor */
    R32(GICD + 0x000) = 0x01;            /* CTLR: enable                    */
    R32(GICD + 0x080) |= (1u << CNTV_PPI);        /* IGROUPR0 -> group1 (NS) */
    R8 (GICD + 0x400 + CNTV_PPI) = 0x00;          /* IPRIORITYR: highest     */
    R32(GICD + 0x100) = (1u << CNTV_PPI);         /* ISENABLER0: enable PPI  */
}
uint32_t kr_irq_claim(void){ return R32(GICC + 0x0C) & 0x3FF; }   /* IAR  */
void     kr_irq_eoi(uint32_t id){ R32(GICC + 0x10) = id; }        /* EOIR */

#else
/* ============= BCM2836 / BCM2837 (Pi 2 / Pi 3): local controller ======== */
#define CORE0_TIMER_IRQCNTL 0x40000040UL
#define CORE0_IRQ_SOURCE    0x40000060UL

void kr_irqctrl_init(void)
{
    R32(CORE0_TIMER_IRQCNTL) = (1u << 3);   /* route nCNTVIRQ to CORE0 IRQ  */
}
uint32_t kr_irq_claim(void){ return R32(CORE0_IRQ_SOURCE); }
void     kr_irq_eoi(uint32_t id){ (void)id; }
#endif
