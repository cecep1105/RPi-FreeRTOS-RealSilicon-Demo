#ifndef IRQCTRL_H
#define IRQCTRL_H
#include <stdint.h>
/* Board-abstracted interrupt controller: BCM2836/7 local controller, or
   BCM2711 GIC-400. Called from the FreeRTOS IRQ handler (portASM.S). */
void     kr_irqctrl_init(void);   /* route/enable the CNTV timer interrupt */
uint32_t kr_irq_claim(void);      /* acknowledge: returns interrupt id     */
void     kr_irq_eoi(uint32_t id); /* end-of-interrupt                      */
#endif
