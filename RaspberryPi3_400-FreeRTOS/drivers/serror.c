#include <stdint.h>
#include "uart.h"
#include "serror.h"

volatile uint32_t g_serror_count = 0;
volatile uint64_t g_serror_esr   = 0;
volatile uint64_t g_serror_far   = 0;
volatile uint64_t g_serror_elr   = 0;

static void ph64(uint64_t v)
{
    uart_puts("0x");
    for (int i = 15; i >= 0; i--) {
        uint32_t n = (v >> (i * 4)) & 0xF;
        uart_putc(n < 10 ? (char)('0' + n) : (char)('A' + n - 10));
    }
}

/* Called from the asm SError vector; records syndrome and returns so the
 * handler can ERET to recover. Print capped to avoid flooding the console. */
void serror_report(uint64_t esr, uint64_t far, uint64_t elr)
{
    g_serror_count++;
    g_serror_esr = esr;
    g_serror_far = far;
    g_serror_elr = elr;

    if (g_serror_count <= 8) {
        uart_puts("\r\n*** SError caught: ESR="); ph64(esr);
        uart_puts(" FAR=");                        ph64(far);
        uart_puts(" ELR=");                        ph64(elr);
        uart_puts("  (recovered)\r\n");
    }
}