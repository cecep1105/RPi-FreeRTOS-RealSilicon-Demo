#ifndef GENET_H
#define GENET_H

#include <stdint.h>
#include "peripherals.h"          /* PERIPHERAL_BASE */

/* Broadcom GENET (UniMAC) Gigabit Ethernet controller.
 *
 * Present only on the BCM2711 (Pi 4 / Pi 400), where it is a native,
 * memory-mapped MAC + external BCM54213PE GPHY. On the BCM2837 (Pi 2/3) the
 * Ethernet is a USB device behind the internal hub and this block does not
 * exist -- there, genet_probe() is a harmless no-op.
 *
 * Register offsets below are hardware facts (Broadcom UniMAC layout); only a
 * few are used at this stage. The rest are defined now so later steps (MDIO,
 * PHY, DMA rings) don't have to keep re-deriving them.
 */

#if (PERIPHERAL_BASE == 0xFE000000UL)
/* Low-peripheral-mode ARM physical base (legacy bus addr 0x7d580000). */
#define GENET_BASE                0xFD580000UL
#else
#define GENET_BASE                0UL        /* not present on this board */
#endif

/* ---- SYS block ---- */
#define GENET_SYS_OFF             0x0000u
#define SYS_REV_CTRL              (GENET_SYS_OFF + 0x00u)
#define SYS_PORT_CTRL             (GENET_SYS_OFF + 0x04u)
#define SYS_RBUF_FLUSH_CTRL       (GENET_SYS_OFF + 0x08u)
#define SYS_TBUF_FLUSH_CTRL       (GENET_SYS_OFF + 0x0Cu)

/* ---- EXT block (RGMII out-of-band control to the external GPHY) ---- */
#define GENET_EXT_OFF             0x0080u
#define EXT_RGMII_OOB_CTRL        (GENET_EXT_OFF + 0x0Cu)

/* ---- RBUF block ---- */
#define GENET_RBUF_OFF            0x0300u
#define RBUF_CTRL                 (GENET_RBUF_OFF + 0x00u)
#define RBUF_TBUF_SIZE_CTRL       (GENET_RBUF_OFF + 0xB4u)

/* ---- UMAC block ---- */
#define GENET_UMAC_OFF            0x0800u
#define UMAC_CMD                  (GENET_UMAC_OFF + 0x008u)
#define UMAC_MAC0                 (GENET_UMAC_OFF + 0x00Cu)   /* addr[0..3] */
#define UMAC_MAC1                 (GENET_UMAC_OFF + 0x010u)   /* addr[4..5] */
#define UMAC_MAX_FRAME_LEN        (GENET_UMAC_OFF + 0x014u)
#define UMAC_MIB_CTRL             (GENET_UMAC_OFF + 0x580u)
#define UMAC_MDIO_CMD             (GENET_UMAC_OFF + 0x614u)

/* ---- DMA blocks (used in later steps) ---- */
#define GENET_RX_OFF              0x2000u
#define GENET_TX_OFF              0x4000u

/* UMAC_CMD bits */
#define CMD_TX_EN                 (1u << 0)
#define CMD_RX_EN                 (1u << 1)
#define CMD_SW_RESET              (1u << 13)

/* ---- MMIO accessors (GENET is Device-nGnRnE: strongly ordered) ---- */
static inline uint32_t genet_rd(uint32_t off)
{
    return *(volatile uint32_t *)(GENET_BASE + off);
}
static inline void genet_wr(uint32_t off, uint32_t val)
{
    *(volatile uint32_t *)(GENET_BASE + off) = val;
    __asm__ volatile("dsb sy" ::: "memory");   /* complete before next step */
}

/* Read core revision + MAC and report over UART.
 * Returns the GENET major version (5 on the Pi 400), or -1 if absent/unreachable. */
int genet_probe(void);

/* ---- Driver API used by the network stack glue (net.c) ---- */
/* Full hardware bring-up: rev check, UMAC reset, RGMII/PHY link, RX+TX DMA.
 * Idempotent. Returns negotiated link speed in Mbps (0 if link down). */
int    genet_net_init(void);
/* Copy one received Ethernet frame (no 2-byte pad, no FCS) into dst.
 * Returns frame length, or 0 if none pending. */
unsigned long genet_net_rx(void *dst, unsigned long maxlen);
/* Transmit one Ethernet frame (hardware appends FCS). Returns len, or 0 on failure. */
unsigned long genet_net_tx(const void *src, unsigned long len);
/* Live PHY link state: 1 = up, 0 = down. */
int    genet_net_up(void);
/* Copy the 6-byte MAC this interface uses. */
void   genet_get_mac(unsigned char mac[6]);

#endif /* GENET_H */