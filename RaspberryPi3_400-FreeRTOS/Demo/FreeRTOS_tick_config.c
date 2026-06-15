/* FreeRTOS includes. */
#include "FreeRTOS.h"
#include "task.h"

#include "demo.h"
#include "peripherals.h"
#include "irqctrl.h"

/* ARM Generic Timer */
static uint32_t timer_cntfrq = 0;
static uint32_t timer_tick = 0;

void enable_cntv(void)
{
	uint32_t cntv_ctl;
	cntv_ctl = 1;
	asm volatile ("msr cntv_ctl_el0, %0" :: "r" (cntv_ctl));
	asm volatile ("isb" ::: "memory");
}
/*-----------------------------------------------------------*/

void write_cntv_tval(uint32_t val)
{
	asm volatile ("msr cntv_tval_el0, %0" :: "r" (val));
	asm volatile ("isb" ::: "memory");
	return;
}
/*-----------------------------------------------------------*/

uint32_t read_cntfrq(void)
{
	uint32_t val;
	asm volatile ("mrs %0, cntfrq_el0" : "=r" (val));
	return val;
}
/*-----------------------------------------------------------*/

void init_timer(void)
{
	timer_cntfrq = timer_tick = read_cntfrq();
	write_cntv_tval(timer_cntfrq);    // clear cntv interrupt and set next 1 sec timer.
	return;
}
/*-----------------------------------------------------------*/

void timer_set_tick_rate_hz(uint32_t rate)
{
	timer_tick = timer_cntfrq / rate ;
	write_cntv_tval(timer_tick);
}
/*-----------------------------------------------------------*/

void vConfigureTickInterrupt( void )
{
	/* init timer device. */
	init_timer();

	/* set tick rate. */
	timer_set_tick_rate_hz(configTICK_RATE_HZ);

	/* interrupt-controller routing (BCM local ctrl or GIC-400). */
	kr_irqctrl_init();

	/* start & enable interrupts in the timer. */
	enable_cntv();
}
/*-----------------------------------------------------------*/

void vClearTickInterrupt( void )
{
	write_cntv_tval(timer_tick);    // clear cntv interrupt and set next timer.
	return;
}
/*-----------------------------------------------------------*/

/* --- non-blocking UART trace (safe in IRQ context: drops if FIFO full) --- */
#define KR_UART_DR (*(volatile uint32_t *)(PERIPHERAL_BASE + 0x201000UL))
#define KR_UART_FR (*(volatile uint32_t *)(PERIPHERAL_BASE + 0x201018UL))
static inline void kr_poke(char c){ if(!(KR_UART_FR & (1u<<5))) KR_UART_DR=(uint32_t)(uint8_t)c; }
static inline void kr_pokehex(uint32_t v){ kr_poke('<'); for(int i=7;i>=0;i--){uint32_t n=(v>>(i*4))&0xf; kr_poke(n<10?('0'+n):('a'+n-10));} kr_poke('>'); }

volatile unsigned int g_irqfire=0;
void vApplicationIRQHandler( uint32_t id )
{
	g_irqfire++;
#if (PERIPHERAL_BASE == 0xFE000000UL)
	/* BCM2711 GIC-400: id is the INTID. CNTV virtual-timer PPI = 27. */
	if(id == 27)            { FreeRTOS_Tick_Handler(); }
	else if(id < 1020)      { irq_handler(); }   /* other SPI/PPI */
	/* 1020-1023 = spurious, ignore */
#else
	/* BCM2836/7 local controller: id is the CORE0 IRQ source bitmask. */
	uint32_t iid = id & 0x0007FFFFUL;
	if(iid & (1 << 3))      { FreeRTOS_Tick_Handler(); }
	else if(iid & (1 << 8)) { irq_handler(); }
#endif
}

