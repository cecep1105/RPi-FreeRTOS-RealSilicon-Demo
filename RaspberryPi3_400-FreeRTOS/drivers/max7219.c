#include "max7219.h"
#include "spi.h"

void max_all(uint8_t reg, uint8_t val) {
    uint8_t buf[8];
    for (int c = 0; c < 4; c++) { buf[c * 2] = reg; buf[c * 2 + 1] = val; }
    spi_write(buf, 8);
}

/* framebuffer: 32 columns, each a byte (bit r = pixel at row r, 0=top) */
void max_render(const uint8_t fb[32]) {
    for (int r = 0; r < 8; r++) {                      /* r = physical row */
        int lrow = MAT_FLIP_Y ? (7 - r) : r;
        uint8_t buf[8];
        for (int c = 0; c < 4; c++) {
            int chip = MAT_REVERSE_CHIPS ? (3 - c) : c;
            uint8_t rb = 0;
            for (int j = 0; j < 8; j++) {
                int col = c * 8 + j;
                if ((fb[col] >> lrow) & 1) {
                    int pos = MAT_MIRROR_X ? j : (7 - j);
                    rb |= (uint8_t)(1u << pos);
                }
            }
            buf[chip * 2]     = (uint8_t)(r + 1);
            buf[chip * 2 + 1] = rb;
        }
        spi_write(buf, 8);
    }
}

void max_init(void) {
    spi_init();
    max_all(0x0F, 0x00);            /* display-test off            */
    max_all(0x09, 0x00);            /* no BCD decode (matrix mode) */
    max_all(0x0B, 0x07);            /* scan all 8 rows             */
    max_all(0x0A, MAT_INTENSITY);   /* brightness                  */
    max_all(0x0C, 0x01);            /* normal operation            */
}
