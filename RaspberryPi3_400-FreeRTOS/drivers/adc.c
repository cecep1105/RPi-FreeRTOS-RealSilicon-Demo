#include "adc.h"
#include "spi.h"
uint16_t mcp3008_read(uint8_t ch)
{
    uint8_t tx[3] = { 0x01, (uint8_t)(0x80 | ((ch & 7) << 4)), 0x00 };
    uint8_t rx[3] = { 0, 0, 0 };
    spi_xfer1(tx, rx, 3);
    return (uint16_t)(((rx[1] & 0x03) << 8) | rx[2]);
}
