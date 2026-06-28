/* rtos_hooks.c -- FreeRTOS safety hooks (Pi 1 bring-up).
 *
 * Turns the two classic silent-corruption failures into a clear UART message
 * instead of a mystery freeze:
 *   - stack overflow  -> names the offending task, then halts
 *   - heap exhaustion -> reports it, then halts
 *
 * Requires in FreeRTOSConfig.h:
 *   #define configCHECK_FOR_STACK_OVERFLOW   2
 *   #define configUSE_MALLOC_FAILED_HOOK     1
 *
 * Leave these on during bring-up; they cost a little per context switch and
 * are cheap insurance. Drop the file (and the two defines) once stable.
 */

#include "FreeRTOS.h"
#include "task.h"
#include "uart.h"

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask;
    uart_puts("\r\n*** STACK OVERFLOW in task: ");
    uart_puts(pcTaskName ? pcTaskName : "(unknown)");
    uart_puts(" -- increase its stack ***\r\n");
    for (;;) { }
}

void vApplicationMallocFailedHook(void)
{
    uart_puts("\r\n*** MALLOC FAILED -- FreeRTOS heap exhausted, "
              "raise configTOTAL_HEAP_SIZE ***\r\n");
    for (;;) { }
}