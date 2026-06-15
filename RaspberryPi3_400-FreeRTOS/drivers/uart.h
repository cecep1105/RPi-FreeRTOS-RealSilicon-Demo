#ifndef UART_H
#define UART_H

void uart_init(void);
int  uart_getc_nb(void);   /* non-blocking: byte 0..255, or -1 if RX empty */
void uart_putc(char c);    /* blocking: waits while TX FIFO is full */
void uart_puts(const char *s);

#endif /* UART_H */
