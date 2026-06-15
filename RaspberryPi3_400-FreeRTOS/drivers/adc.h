#ifndef ADC_H
#define ADC_H
#include <stdint.h>
/* MCP3008 10-bit ADC on SPI0 CE1 (GPIO7). Returns 0..1023, ch 0..7. */
uint16_t mcp3008_read(uint8_t ch);
#endif
