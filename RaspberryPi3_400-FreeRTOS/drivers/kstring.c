#include <stddef.h>
#include <stdint.h>
/* Freestanding mem* not already provided by the FreeRTOS port (port.c has memcpy). */
void *memset(void *s, int c, size_t n){ uint8_t *p=s; while(n--) *p++=(uint8_t)c; return s; }
