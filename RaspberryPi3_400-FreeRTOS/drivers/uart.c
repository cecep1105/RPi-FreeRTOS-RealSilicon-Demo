#include "uart.h"
#include "peripherals.h"
#include "gpio.h"

#define UART0_BASE (PERIPHERAL_BASE + 0x201000UL)
#define UART0_DR   ((volatile uint32_t *)(UART0_BASE + 0x00))
#define UART0_FR   ((volatile uint32_t *)(UART0_BASE + 0x18))
#define UART0_IBRD ((volatile uint32_t *)(UART0_BASE + 0x24))
#define UART0_FBRD ((volatile uint32_t *)(UART0_BASE + 0x28))
#define UART0_LCRH ((volatile uint32_t *)(UART0_BASE + 0x2C))
#define UART0_CR   ((volatile uint32_t *)(UART0_BASE + 0x30))
#define UART0_ICR  ((volatile uint32_t *)(UART0_BASE + 0x44))

void uart_init(void) {
    *UART0_CR = 0;
    gpio_set_function(14, GPIO_FUNC_ALT0);   /* TXD0 */
    gpio_set_function(15, GPIO_FUNC_ALT0);   /* RXD0 */
    *UART0_ICR  = 0x7FF;
    *UART0_IBRD = 26;                        /* 115200 @ 48 MHz UARTCLK */
    *UART0_FBRD = 3;
    *UART0_LCRH = (1u << 4) | (3u << 5);     /* FEN | 8-bit */
    *UART0_CR   = (1u << 0) | (1u << 8) | (1u << 9);  /* UARTEN | TXE | RXE */
}

int uart_getc_nb(void) {
    if (*UART0_FR & (1u << 4)) return -1;    /* RXFE */
    return (int)(*UART0_DR & 0xFF);
}

void uart_putc(char c) {
    while (*UART0_FR & (1u << 5)) { }        /* wait while TXFF (TX FIFO full) */
    *UART0_DR = (uint32_t)(uint8_t)c;
}

void uart_puts(const char *s) {
    while (*s) uart_putc(*s++);
}
