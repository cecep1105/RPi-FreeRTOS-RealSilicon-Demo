#include <stdint.h>
#include "genet.h"
#include "uart.h"
#include "timer.h"                 /* delay_us(), sys_us() */

#if (PERIPHERAL_BASE == 0xFE000000UL)
/* ===================== BCM2711 (Pi 4 / Pi 400) ========================= */

/* --- UMAC reset / RGMII / MDIO bit fields (hardware facts) --- */
#define SYS_RBUF_FLUSH_RESET   (1u << 1)
#define CMD_LCL_LOOP_EN        (1u << 15)
#define MIB_RESET_RX           (1u << 0)
#define MIB_RESET_RUNT         (1u << 1)
#define MIB_RESET_TX           (1u << 2)
#define RBUF_ALIGN_2B          (1u << 1)
#define ENET_MAX_MTU_SIZE      1536u

#define MDIO_START_BUSY        (1u << 29)
#define MDIO_READ_FAIL         (1u << 28)
#define MDIO_RD                (2u << 26)
#define MDIO_WR                (1u << 26)
#define MDIO_PMD_SHIFT         21
#define MDIO_REG_SHIFT         16
#define GENET_PHY_ADDR         1u

#define PORT_MODE_EXT_GPHY     3u
#define RGMII_LINK             (1u << 4)
#define OOB_DISABLE            (1u << 5)
#define RGMII_MODE_EN          (1u << 6)
#define ID_MODE_DIS            (1u << 16)
#define UMAC_SPEED_10          0u
#define UMAC_SPEED_100         1u
#define UMAC_SPEED_1000        2u
#define CMD_SPEED_SHIFT        2
#define CMD_SPEED_MASK         3u

#define MII_BMCR               0x00
#define MII_BMSR               0x01
#define MII_STAT1000           0x0A
#define BCM_AUX_STAT           0x19
#define BMCR_ANRESTART         (1u << 9)
#define BMCR_ANENABLE          (1u << 12)
#define BMSR_LSTATUS           (1u << 2)

/* --- DMA ring layout (descriptor RAM is fixed at 256 slots) --- */
#define TOTAL_DESCS            256u
#define RX_DESCS              256u
#define TX_DESCS              256u
#define DEFAULT_Q             16u
#define DMA_DESC_SIZE         12u
#define DMA_RING_SIZE         0x40u
#define RX_BUF_LENGTH        2048u
#define RX_BUF_OFFSET           2u

#define GENET_RDMA_REG_OFF   (GENET_RX_OFF + TOTAL_DESCS * DMA_DESC_SIZE)
#define RDMA_RING_REG_BASE   (GENET_RDMA_REG_OFF + DEFAULT_Q * DMA_RING_SIZE)
#define RDMA_REG_BASE        (GENET_RDMA_REG_OFF + DMA_RING_SIZE * (DEFAULT_Q+1))
#define GENET_TDMA_REG_OFF   (GENET_TX_OFF + TOTAL_DESCS * DMA_DESC_SIZE)
#define TDMA_RING_REG_BASE   (GENET_TDMA_REG_OFF + DEFAULT_Q * DMA_RING_SIZE)
#define TDMA_REG_BASE        (GENET_TDMA_REG_OFF + DMA_RING_SIZE * (DEFAULT_Q+1))

#define DMA_DESC_LENGTH_STATUS 0x00u
#define DMA_DESC_ADDRESS_LO    0x04u
#define DMA_DESC_ADDRESS_HI    0x08u
#define DMA_OWN               0x8000u
#define DMA_BUFLENGTH_SHIFT      16
#define DMA_BUFLENGTH_MASK    0x0FFFu
#define DMA_TX_QTAG_SHIFT        7
#define DMA_TX_APPEND_CRC    0x0040u
#define DMA_SOP              0x2000u
#define DMA_EOP              0x4000u

#define R_RING_WRITE_PTR      0x00u
#define R_RING_PROD_INDEX     0x08u
#define R_RING_CONS_INDEX     0x0Cu
#define R_RING_BUF_SIZE       0x10u
#define R_RING_START_ADDR     0x14u
#define R_RING_END_ADDR       0x1Cu
#define R_RING_XON_XOFF       0x28u
#define R_RING_READ_PTR       0x2Cu
#define T_RING_READ_PTR       0x00u
#define T_RING_CONS_INDEX     0x08u
#define T_RING_PROD_INDEX     0x0Cu
#define T_RING_MBUF_DONE      0x24u
#define T_RING_FLOW_PERIOD    0x28u
#define T_RING_WRITE_PTR      0x2Cu

#define R_DMA_RING_CFG        0x00u
#define R_DMA_CTRL            0x04u
#define R_DMA_SCB_BURST       0x0Cu
#define DMA_EN                   1u
#define DMA_RING_BUF_EN_SHIFT    1
#define DMA_RING_SIZE_SHIFT     16
#define DMA_MAX_BURST_LENGTH     8u
#define DMA_FC_THRESH_VALUE  ((5u << 16) | (RX_DESCS >> 4))

static uint8_t  g_rxbuf[RX_DESCS * RX_BUF_LENGTH] __attribute__((aligned(64)));
static uint32_t g_rx_cindex, g_rx_index, g_tx_index;
static const uint8_t g_my_mac[6] = { 0x02, 0xCA, 0xFE, 0x00, 0x00, 0x01 };

static void put_hex32(uint32_t v)
{
    uart_puts("0x");
    for (int i = 7; i >= 0; i--) {
        uint32_t n = (v >> (i * 4)) & 0xF;
        uart_putc(n < 10 ? (char)('0' + n) : (char)('A' + n - 10));
    }
}
static void put_dec(uint32_t v)
{
    char b[11]; int i = 10; b[10] = 0;
    if (!v) { uart_puts("0"); return; }
    while (v && i) { b[--i] = (char)('0' + (v % 10)); v /= 10; }
    uart_puts(&b[i]);
}

static void genet_umac_reset(void)
{
    uint32_t reg = genet_rd(SYS_RBUF_FLUSH_CTRL);
    genet_wr(SYS_RBUF_FLUSH_CTRL, reg | SYS_RBUF_FLUSH_RESET);
    delay_us(10);
    genet_wr(SYS_RBUF_FLUSH_CTRL, reg & ~SYS_RBUF_FLUSH_RESET);
    delay_us(10);

    genet_wr(UMAC_CMD, 0);
    genet_wr(UMAC_CMD, CMD_SW_RESET | CMD_LCL_LOOP_EN);
    delay_us(2);
    genet_wr(UMAC_CMD, 0);

    genet_wr(UMAC_MIB_CTRL, MIB_RESET_RX | MIB_RESET_TX | MIB_RESET_RUNT);
    genet_wr(UMAC_MIB_CTRL, 0);
    genet_wr(UMAC_MAX_FRAME_LEN, ENET_MAX_MTU_SIZE);

    reg = genet_rd(RBUF_CTRL);
    genet_wr(RBUF_CTRL, reg | RBUF_ALIGN_2B);
    genet_wr(RBUF_TBUF_SIZE_CTRL, 1);
}

static int genet_mdio_read(uint32_t phy, uint32_t reg)
{
    uint32_t cmd = MDIO_RD | (phy << MDIO_PMD_SHIFT) | (reg << MDIO_REG_SHIFT);
    genet_wr(UMAC_MDIO_CMD, cmd);
    genet_wr(UMAC_MDIO_CMD, genet_rd(UMAC_MDIO_CMD) | MDIO_START_BUSY);
    uint64_t t0 = sys_us();
    while (genet_rd(UMAC_MDIO_CMD) & MDIO_START_BUSY)
        if (sys_us() - t0 > 100000ULL) return -1;
    uint32_t v = genet_rd(UMAC_MDIO_CMD);
    if (v & MDIO_READ_FAIL) return -2;
    return (int)(v & 0xFFFFu);
}

static int genet_mdio_write(uint32_t phy, uint32_t reg, uint32_t val)
{
    uint32_t cmd = MDIO_WR | (phy << MDIO_PMD_SHIFT) | (reg << MDIO_REG_SHIFT) | (val & 0xFFFFu);
    genet_wr(UMAC_MDIO_CMD, cmd);
    genet_wr(UMAC_MDIO_CMD, genet_rd(UMAC_MDIO_CMD) | MDIO_START_BUSY);
    uint64_t t0 = sys_us();
    while (genet_rd(UMAC_MDIO_CMD) & MDIO_START_BUSY)
        if (sys_us() - t0 > 100000ULL) return -1;
    return 0;
}

static int genet_link_bringup(void)
{
    genet_wr(SYS_PORT_CTRL, PORT_MODE_EXT_GPHY);

    int bmcr = genet_mdio_read(GENET_PHY_ADDR, MII_BMCR);
    if (bmcr < 0) return -1;
    genet_mdio_write(GENET_PHY_ADDR, MII_BMCR,
                     (uint32_t)bmcr | BMCR_ANENABLE | BMCR_ANRESTART);

    int up = 0;
    uint64_t t0 = sys_us();
    while (sys_us() - t0 < 4000000ULL) {
        (void)genet_mdio_read(GENET_PHY_ADDR, MII_BMSR);
        int bmsr = genet_mdio_read(GENET_PHY_ADDR, MII_BMSR);
        if (bmsr < 0) return -2;
        if ((uint32_t)bmsr & BMSR_LSTATUS) { up = 1; break; }
        delay_us(50000);
    }
    if (!up) return 0;

    int aux = genet_mdio_read(GENET_PHY_ADDR, BCM_AUX_STAT);
    int s1k = genet_mdio_read(GENET_PHY_ADDR, MII_STAT1000);
    int speed = 0;
    switch (((uint32_t)aux >> 8) & 0x7u) {
        case 7: case 6: speed = 1000; break;
        case 5: case 4: case 3: speed = 100; break;
        case 2: case 1: speed = 10; break;
        default: speed = 0; break;
    }
    if (speed == 0 && ((uint32_t)s1k & (3u << 10))) speed = 1000;

    uint32_t reg = genet_rd(EXT_RGMII_OOB_CTRL);
    reg &= ~OOB_DISABLE;
    reg |= RGMII_LINK | RGMII_MODE_EN | ID_MODE_DIS;
    genet_wr(EXT_RGMII_OOB_CTRL, reg);

    uint32_t sp = (speed == 1000) ? UMAC_SPEED_1000
                : (speed == 100)  ? UMAC_SPEED_100 : UMAC_SPEED_10;
    reg = genet_rd(UMAC_CMD);
    reg &= ~(CMD_SPEED_MASK << CMD_SPEED_SHIFT);
    reg |= (sp << CMD_SPEED_SHIFT);
    genet_wr(UMAC_CMD, reg);

    return speed;
}

/* ---- RX ring ---- */
static uint32_t rdesc(uint32_t i, uint32_t f) { return genet_rd(GENET_RX_OFF + i*DMA_DESC_SIZE + f); }
static void     wdesc(uint32_t i, uint32_t f, uint32_t v) { genet_wr(GENET_RX_OFF + i*DMA_DESC_SIZE + f, v); }

static void genet_rx_setup(void)
{
    uint32_t r = genet_rd(RDMA_REG_BASE + R_DMA_CTRL);
    genet_wr(RDMA_REG_BASE + R_DMA_CTRL, r & ~DMA_EN);

    for (uint32_t i = 0; i < RX_DESCS; i++) {
        uintptr_t b = (uintptr_t)&g_rxbuf[i * RX_BUF_LENGTH];
        wdesc(i, DMA_DESC_ADDRESS_LO, (uint32_t)(b & 0xFFFFFFFFu));
        wdesc(i, DMA_DESC_ADDRESS_HI, (uint32_t)((uint64_t)b >> 32));
        wdesc(i, DMA_DESC_LENGTH_STATUS, (RX_BUF_LENGTH << DMA_BUFLENGTH_SHIFT) | DMA_OWN);
    }

    genet_wr(RDMA_REG_BASE + R_DMA_SCB_BURST, DMA_MAX_BURST_LENGTH);
    genet_wr(RDMA_RING_REG_BASE + R_RING_START_ADDR, 0);
    genet_wr(RDMA_RING_REG_BASE + R_RING_READ_PTR,   0);
    genet_wr(RDMA_RING_REG_BASE + R_RING_WRITE_PTR,  0);
    genet_wr(RDMA_RING_REG_BASE + R_RING_END_ADDR, RX_DESCS * DMA_DESC_SIZE / 4 - 1);

    g_rx_cindex = genet_rd(RDMA_RING_REG_BASE + R_RING_PROD_INDEX);
    genet_wr(RDMA_RING_REG_BASE + R_RING_CONS_INDEX, g_rx_cindex);
    g_rx_index = g_rx_cindex & 0xFFu;

    genet_wr(RDMA_RING_REG_BASE + R_RING_BUF_SIZE, (RX_DESCS << DMA_RING_SIZE_SHIFT) | RX_BUF_LENGTH);
    genet_wr(RDMA_RING_REG_BASE + R_RING_XON_XOFF, DMA_FC_THRESH_VALUE);
    genet_wr(RDMA_REG_BASE + R_DMA_RING_CFG, 1u << DEFAULT_Q);

    uint32_t dma_ctrl = (1u << (DEFAULT_Q + DMA_RING_BUF_EN_SHIFT)) | DMA_EN;
    r = genet_rd(RDMA_REG_BASE + R_DMA_CTRL);
    genet_wr(RDMA_REG_BASE + R_DMA_CTRL, r | dma_ctrl);
}

static int genet_rx_poll(uint8_t **buf)
{
    uint32_t prod = genet_rd(RDMA_RING_REG_BASE + R_RING_PROD_INDEX) & 0xFFFFu;
    if (prod == (g_rx_cindex & 0xFFFFu)) return 0;
    __asm__ volatile("dsb sy" ::: "memory");
    uint32_t ls  = rdesc(g_rx_index, DMA_DESC_LENGTH_STATUS);
    uint32_t len = (ls >> DMA_BUFLENGTH_SHIFT) & DMA_BUFLENGTH_MASK;
    uint32_t lo  = rdesc(g_rx_index, DMA_DESC_ADDRESS_LO);
    *buf = (uint8_t *)(uintptr_t)lo;
    return (int)len;
}

static void genet_rx_release(void)
{
    g_rx_cindex = (g_rx_cindex + 1) & 0xFFFFu;
    genet_wr(RDMA_RING_REG_BASE + R_RING_CONS_INDEX, g_rx_cindex);
    if (++g_rx_index >= RX_DESCS) g_rx_index = 0;
}

/* ---- TX ring ---- */
static void genet_tx_init(void)
{
    uint32_t r = genet_rd(TDMA_REG_BASE + R_DMA_CTRL);
    genet_wr(TDMA_REG_BASE + R_DMA_CTRL, r & ~DMA_EN);

    genet_wr(TDMA_REG_BASE + R_DMA_SCB_BURST, DMA_MAX_BURST_LENGTH);
    genet_wr(TDMA_RING_REG_BASE + R_RING_START_ADDR, 0);
    genet_wr(TDMA_RING_REG_BASE + T_RING_READ_PTR,   0);
    genet_wr(TDMA_RING_REG_BASE + T_RING_WRITE_PTR,  0);
    genet_wr(TDMA_RING_REG_BASE + R_RING_END_ADDR, TX_DESCS * DMA_DESC_SIZE / 4 - 1);

    g_tx_index = genet_rd(TDMA_RING_REG_BASE + T_RING_CONS_INDEX);
    genet_wr(TDMA_RING_REG_BASE + T_RING_PROD_INDEX, g_tx_index);
    g_tx_index &= 0xFFu;

    genet_wr(TDMA_RING_REG_BASE + T_RING_MBUF_DONE,  1);
    genet_wr(TDMA_RING_REG_BASE + T_RING_FLOW_PERIOD, 0);
    genet_wr(TDMA_RING_REG_BASE + R_RING_BUF_SIZE, (TX_DESCS << DMA_RING_SIZE_SHIFT) | RX_BUF_LENGTH);
    genet_wr(TDMA_REG_BASE + R_DMA_RING_CFG, 1u << DEFAULT_Q);

    uint32_t dma_ctrl = (1u << (DEFAULT_Q + DMA_RING_BUF_EN_SHIFT)) | DMA_EN;
    genet_wr(TDMA_REG_BASE + R_DMA_CTRL, dma_ctrl);
}

static int genet_tx_send(const uint8_t *pkt, uint32_t len)
{
    uint32_t prod = genet_rd(TDMA_RING_REG_BASE + T_RING_PROD_INDEX);
    uintptr_t b = (uintptr_t)pkt;
    uint32_t len_stat = (len << DMA_BUFLENGTH_SHIFT)
                      | (0x3Fu << DMA_TX_QTAG_SHIFT)
                      | DMA_TX_APPEND_CRC | DMA_SOP | DMA_EOP;

    __asm__ volatile("dsb sy" ::: "memory");
    genet_wr(GENET_TX_OFF + g_tx_index*DMA_DESC_SIZE + DMA_DESC_ADDRESS_LO, (uint32_t)(b & 0xFFFFFFFFu));
    genet_wr(GENET_TX_OFF + g_tx_index*DMA_DESC_SIZE + DMA_DESC_ADDRESS_HI, (uint32_t)((uint64_t)b >> 32));
    genet_wr(GENET_TX_OFF + g_tx_index*DMA_DESC_SIZE + DMA_DESC_LENGTH_STATUS, len_stat);

    if (++g_tx_index >= TX_DESCS) g_tx_index = 0;
    prod++;
    genet_wr(TDMA_RING_REG_BASE + T_RING_PROD_INDEX, prod);

    uint64_t t0 = sys_us();
    while ((genet_rd(TDMA_RING_REG_BASE + T_RING_CONS_INDEX) & 0xFFFFu) < prod)
        if (sys_us() - t0 > 100000ULL) return -1;
    return 0;
}

/* ============================ public API ============================== */
int genet_net_init(void)
{
    static int done = 0, speed = 0;
    if (done) return speed;

    uint32_t rev = genet_rd(SYS_REV_CTRL);
    uint32_t major = (rev >> 24) & 0xFu; if (major == 6u) major = 5u;
    uart_puts("GENET: core v"); put_dec(major); uart_puts(".0 init\r\n");

    genet_umac_reset();

    /* program our unicast MAC so the UMAC accepts frames addressed to us */
    genet_wr(UMAC_MAC0, ((uint32_t)g_my_mac[0] << 24) | ((uint32_t)g_my_mac[1] << 16) |
                        ((uint32_t)g_my_mac[2] << 8)  |  (uint32_t)g_my_mac[3]);
    genet_wr(UMAC_MAC1, ((uint32_t)g_my_mac[4] << 8)  |  (uint32_t)g_my_mac[5]);

    int phy = genet_mdio_read(GENET_PHY_ADDR, 2);
    uart_puts("GENET: PHY reg2="); put_hex32((uint32_t)phy); uart_puts("\r\n");

    speed = genet_link_bringup();
    if (speed > 0) { uart_puts("GENET: LINK UP "); put_dec((uint32_t)speed); uart_puts(" Mbps\r\n"); }
    else           uart_puts("GENET: link DOWN at init (check cable)\r\n");

    genet_rx_setup();
    genet_tx_init();

    /* enable MAC RX + TX */
    uint32_t cmd = genet_rd(UMAC_CMD);
    genet_wr(UMAC_CMD, cmd | CMD_RX_EN | CMD_TX_EN);

    done = 1;
    return speed;
}

unsigned long genet_net_rx(void *dst, unsigned long maxlen)
{
    uint8_t *b;
    int dl = genet_rx_poll(&b);
    if (dl <= (int)RX_BUF_OFFSET) return 0;
    unsigned long flen = (unsigned long)dl - RX_BUF_OFFSET;
    if (flen > maxlen) flen = maxlen;
    const uint8_t *s = b + RX_BUF_OFFSET;
    uint8_t *d = (uint8_t *)dst;
    for (unsigned long i = 0; i < flen; i++) d[i] = s[i];
    genet_rx_release();
    return flen;
}

unsigned long genet_net_tx(const void *src, unsigned long len)
{
    if (genet_tx_send((const uint8_t *)src, (uint32_t)len) < 0) return 0;
    return len;
}

int genet_net_up(void)
{
    (void)genet_mdio_read(GENET_PHY_ADDR, MII_BMSR);
    int bmsr = genet_mdio_read(GENET_PHY_ADDR, MII_BMSR);
    return (bmsr >= 0 && ((uint32_t)bmsr & BMSR_LSTATUS)) ? 1 : 0;
}

void genet_get_mac(unsigned char mac[6])
{
    for (int i = 0; i < 6; i++) mac[i] = g_my_mac[i];
}

int genet_probe(void)
{
    int spd = genet_net_init();
    uart_puts("GENET: ready, link ");
    uart_puts(spd > 0 ? "UP\r\n" : "DOWN\r\n");
    uint32_t rev = genet_rd(SYS_REV_CTRL);
    uint32_t major = (rev >> 24) & 0xFu; if (major == 6u) major = 5u;
    return (int)major;
}

#else
/* ===================== BCM2837 (Pi 2 / Pi 3): no GENET ================== */
int genet_probe(void)    { uart_puts("GENET: Pi 4 / Pi 400 only\r\n"); return -1; }
int genet_net_init(void) { return 0; }
unsigned long genet_net_rx(void *d, unsigned long m) { (void)d; (void)m; return 0; }
unsigned long genet_net_tx(const void *s, unsigned long l) { (void)s; (void)l; return 0; }
int genet_net_up(void)   { return 0; }
void genet_get_mac(unsigned char mac[6]) { for (int i=0;i<6;i++) mac[i]=0; }
#endif