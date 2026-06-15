#ifndef DEMO_H
#define DEMO_H
/* Called from the FreeRTOS IRQ dispatcher (FreeRTOS_tick_config.c) for the
   GPU/peripheral IRQ line (bit 8). No peripheral IRQs in this build -> stub. */
void irq_handler(void);
#endif
