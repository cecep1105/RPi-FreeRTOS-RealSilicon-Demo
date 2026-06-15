#ifndef SPI_H
#define SPI_H

#include <stdint.h>

void spi_init(void);
void spi_write(const uint8_t *buf, int len);  /* one CS-framed burst on CE0 */

#endif /* SPI_H */
