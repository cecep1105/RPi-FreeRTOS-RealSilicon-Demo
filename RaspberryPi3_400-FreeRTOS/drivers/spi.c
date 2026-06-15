#include "spi.h"
#include "peripherals.h"
#include "gpio.h"

#define SPI0_BASE  (PERIPHERAL_BASE + 0x204000UL)
#define SPI_CS     ((volatile uint32_t *)(SPI0_BASE + 0x00))
#define SPI_FIFO   ((volatile uint32_t *)(SPI0_BASE + 0x04))
#define SPI_CLK    ((volatile uint32_t *)(SPI0_BASE + 0x08))
#define SPI_CS_TA    (1u << 7)
#define SPI_CS_DONE  (1u << 16)
#define SPI_CS_RXD   (1u << 17)
#define SPI_CS_TXD   (1u << 18)
#define SPI_CS_CLEAR (3u << 4)

void spi_init(void) {
    gpio_set_function(7,  GPIO_FUNC_ALT0);   /* CE1  */
    gpio_set_function(8,  GPIO_FUNC_ALT0);   /* CE0  */
    gpio_set_function(9,  GPIO_FUNC_ALT0);   /* MISO */
    gpio_set_function(10, GPIO_FUNC_ALT0);   /* MOSI */
    gpio_set_function(11, GPIO_FUNC_ALT0);   /* SCLK */
    *SPI_CS  = SPI_CS_CLEAR;
    *SPI_CLK = 256;                          /* ~1 MHz SCLK */
}

void spi_write(const uint8_t *buf, int len) {
    *SPI_CS = SPI_CS_CLEAR;
    *SPI_CS = SPI_CS_TA;                      /* CS=CE0, CPOL/CPHA=0 */
    for (int i = 0; i < len; i++) {
        { uint32_t g=200u; while(!(*SPI_CS & SPI_CS_TXD) && --g){} }
        *SPI_FIFO = buf[i];
        { uint32_t g=200u; while((*SPI_CS & SPI_CS_RXD) && --g) (void)*SPI_FIFO; }
    }
    { uint32_t g=20000u; while(!(*SPI_CS & SPI_CS_DONE) && --g){} }
    *SPI_CS = SPI_CS_CLEAR;                   /* TA=0 -> CS high -> latch */
}

void spi_xfer1(const uint8_t *tx, uint8_t *rx, int len) {  /* full-duplex on CE1 */
    *SPI_CS = SPI_CS_CLEAR | 0x01u;            /* select CE1, clear FIFOs */
    *SPI_CS = SPI_CS_TA   | 0x01u;             /* TA=1, CS=CE1 */
    for (int i = 0; i < len; i++) {
        { uint32_t g=20000u; while(!(*SPI_CS & SPI_CS_TXD) && --g){} }
        *SPI_FIFO = tx[i];
        { uint32_t g=20000u; while(!(*SPI_CS & SPI_CS_RXD) && --g){} }
        rx[i] = (uint8_t)*SPI_FIFO;
    }
    { uint32_t g=20000u; while(!(*SPI_CS & SPI_CS_DONE) && --g){} }
    *SPI_CS = SPI_CS_CLEAR;
}
