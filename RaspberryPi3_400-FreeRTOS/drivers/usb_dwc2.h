#ifndef USB_DWC2_H
#define USB_DWC2_H

#include <stdint.h>
#include "peripherals.h"          /* PERIPHERAL_BASE */

/* Synopsys DesignWare USB 2.0 OTG controller (DWC2 / "dwc_otg").
 *
 * On the BCM2837 (Pi 2 / Pi 3) this is the ONLY USB host, and the board's
 * Ethernet hangs off it: DWC2 root port -> onboard LAN9514 (USB hub + SMSC9512
 * 10/100 MAC). So bringing Ethernet up on the Pi 3 means bringing up this host
 * controller from scratch, enumerating the hub, then driving the MAC.
 *
 * Register window: PERIPHERAL_BASE + 0x980000
 *   Pi 2/3  (BCM2837) : 0x3F980000
 *   Pi 4/400(BCM2711) : 0xFE980000   (present, but those boards use GENET)
 *
 * The core has an INTERNAL DMA engine. Because our MMU maps RAM as
 * Normal-Non-Cacheable with the D-cache off, DMA buffers are coherent with the
 * CPU automatically -- no clean/invalidate, only dsb ordering (same deal that
 * made the GENET rings simple).
 *
 * Offsets below are hardware facts (Synopsys DWC_otg layout). Most are unused
 * at this first step but are defined now so later steps (host init, the EP0
 * channel state machine, the root port) don't keep re-deriving them.
 */

#define USB_BASE                  (PERIPHERAL_BASE + 0x00980000UL)

/* ---- Core Global CSRs ---- */
#define GOTGCTL                   0x000u
#define GOTGINT                   0x004u
#define GAHBCFG                   0x008u
#define GUSBCFG                   0x00Cu
#define GRSTCTL                   0x010u
#define GINTSTS                   0x014u
#define GINTMSK                   0x018u
#define GRXSTSR                   0x01Cu
#define GRXSTSP                   0x020u
#define GRXFSIZ                   0x024u
#define GNPTXFSIZ                 0x028u
#define GNPTXSTS                  0x02Cu
#define GSNPSID                   0x040u   /* "OT" + BCD version (0x4F54xxxx) */
#define GHWCFG1                   0x044u
#define GHWCFG2                   0x048u
#define GHWCFG3                   0x04Cu
#define GHWCFG4                   0x050u
#define GDFIFOCFG                 0x05Cu
#define HPTXFSIZ                  0x100u

/* ---- Host-mode registers ---- */
#define HCFG                      0x400u
#define HFIR                      0x404u
#define HFNUM                     0x408u
#define HPTXSTS                   0x410u
#define HAINT                     0x414u
#define HAINTMSK                  0x418u
#define HPRT0                     0x440u   /* root-port control/status */

/* Per-channel host registers: HC_BASE(ch) + field, 0x20 stride from 0x500 */
#define HC_BASE(ch)               (0x500u + (uint32_t)(ch) * 0x20u)
#define HCCHAR                    0x00u
#define HCSPLT                    0x04u
#define HCINT                     0x08u
#define HCINTMSK                  0x0Cu
#define HCTSIZ                    0x10u
#define HCDMA                     0x14u
#define HCDMAB                    0x1Cu

/* Per-channel data FIFO (slave mode); DMA mode addresses RAM directly. */
#define DFIFO(ch)                 (0x1000u + (uint32_t)(ch) * 0x1000u)

#define PCGCCTL                   0xE00u   /* power & clock gating */

/* ---- HCFG bits ---- */
#define HCFG_FSLSPCLKSEL_MASK     (3u << 0)    /* 0 = 30/60MHz (HS UTMI+)      */
#define HCFG_FSLSSUPP             (1u << 2)    /* 1 = FS/LS only               */

/* ---- GRSTCTL bits ---- */
#define GRSTCTL_CSFTRST           (1u << 0)
#define GRSTCTL_FRMCNTRRST        (1u << 2)
#define GRSTCTL_RXFFLSH           (1u << 4)
#define GRSTCTL_TXFFLSH           (1u << 5)
#define GRSTCTL_TXFNUM_SHIFT      6
#define GRSTCTL_DMAREQ            (1u << 30)
#define GRSTCTL_AHBIDLE           (1u << 31)

/* ---- GUSBCFG bits ---- */
#define GUSBCFG_PHYSEL            (1u << 6)    /* 1 = FS serial PHY            */
#define GUSBCFG_SRPCAP            (1u << 8)
#define GUSBCFG_HNPCAP            (1u << 9)
#define GUSBCFG_FHMOD             (1u << 29)   /* force host mode              */
#define GUSBCFG_FDMOD             (1u << 30)   /* force device mode            */

/* ---- GUSBCFG extra bits (PHY selection / init) ---- */
#define GUSBCFG_PHYIF             (1u << 3)    /* 0 = 8-bit UTMI, 1 = 16-bit   */
#define GUSBCFG_ULPI_UTMI_SEL     (1u << 4)    /* 0 = UTMI+, 1 = ULPI          */
#define GUSBCFG_ULPI_EXT_VBUS_DRV (1u << 20)
#define GUSBCFG_TERM_SEL_DL_PULSE (1u << 22)

/* ---- GAHBCFG bits ---- */
#define GAHBCFG_GLBLINTRMSK       (1u << 0)
#define GAHBCFG_HBSTLEN_SHIFT     1
#define GAHBCFG_DMAEN             (1u << 5)
#define GAHBCFG_NPTXFEMPLVL       (1u << 7)
#define GAHBCFG_PTXFEMPLVL        (1u << 8)

/* ---- GINTSTS bits (subset) ---- */
#define GINTSTS_CURMOD_HOST       (1u << 0)
#define GINTSTS_SOF               (1u << 3)
#define GINTSTS_RXFLVL            (1u << 4)
#define GINTSTS_HPRTINT           (1u << 24)
#define GINTSTS_HCHINT            (1u << 25)

/* ---- HPRT0 bits (note: 1,3,5 are write-1-to-clear change bits) ---- */
#define HPRT0_CONNSTS             (1u << 0)
#define HPRT0_CONNDET             (1u << 1)    /* W1C */
#define HPRT0_ENA                 (1u << 2)    /* W1C-to-disable; preserve!    */
#define HPRT0_ENCHNG              (1u << 3)    /* W1C */
#define HPRT0_OCACT               (1u << 4)
#define HPRT0_OCCHNG              (1u << 5)    /* W1C */
#define HPRT0_RST                 (1u << 8)
#define HPRT0_PWR                 (1u << 12)
#define HPRT0_SPD_SHIFT           17           /* 0=HS 1=FS 2=LS */
#define HPRT0_SPD_MASK            (3u << 17)
/* Mask of W1C change bits to clear when doing a read-modify-write of HPRT0. */
#define HPRT0_WC_BITS             (HPRT0_CONNDET | HPRT0_ENA | HPRT0_ENCHNG | HPRT0_OCCHNG)

/* ---- HCCHAR (host channel characteristics) fields ---- */
#define HCCHAR_MPS_MASK           0x7FFu          /* [10:0]  max packet size   */
#define HCCHAR_EPNUM_SHIFT        11              /* [14:11] endpoint number   */
#define HCCHAR_EPDIR_IN           (1u << 15)      /* 1 = IN                    */
#define HCCHAR_LSPDDEV            (1u << 17)      /* 1 = low-speed device      */
#define HCCHAR_EPTYPE_SHIFT       18              /* [19:18] endpoint type     */
#define HCCHAR_MC_SHIFT           20              /* [21:20] multi-count (=1)  */
#define HCCHAR_DEVADDR_SHIFT      22              /* [28:22] device address    */
#define HCCHAR_ODDFRM             (1u << 29)
#define HCCHAR_CHDIS              (1u << 30)
#define HCCHAR_CHENA              (1u << 31)

/* ---- HCTSIZ (host channel transfer size) fields ---- */
#define HCTSIZ_XFERSIZE_MASK      0x7FFFFu        /* [18:0]                    */
#define HCTSIZ_PKTCNT_SHIFT       19              /* [28:19]                   */
#define HCTSIZ_PID_SHIFT          29              /* [30:29] data PID          */
#define HCTSIZ_DOPING             (1u << 31)

/* ---- HCINT / HCINTMSK bits ---- */
#define HCINT_XFERCOMPL           (1u << 0)
#define HCINT_CHHLTD              (1u << 1)
#define HCINT_AHBERR              (1u << 2)
#define HCINT_STALL               (1u << 3)
#define HCINT_NAK                 (1u << 4)
#define HCINT_ACK                 (1u << 5)
#define HCINT_NYET                (1u << 6)
#define HCINT_XACTERR             (1u << 7)
#define HCINT_BBLERR              (1u << 8)
#define HCINT_FRMOVRUN            (1u << 9)
#define HCINT_DATATGLERR          (1u << 10)

/* ---- Endpoint types (HCCHAR EPTYPE) ---- */
#define EPTYPE_CONTROL            0u
#define EPTYPE_ISO                1u
#define EPTYPE_BULK               2u
#define EPTYPE_INTR               3u

/* ---- Data PIDs (HCTSIZ PID) ---- */
#define USB_PID_DATA0             0u
#define USB_PID_DATA2             1u
#define USB_PID_DATA1             2u
#define USB_PID_SETUP             3u

/* DWC2 DMA sees RAM through the VideoCore interconnect: program HCDMA with the
 * *bus* alias of the buffer, not the ARM physical address. The correct alias
 * differs by board because the ARM reaches SDRAM differently:
 *   - Pi 3/400 (AArch64): ARM caches are on but the USB buffers are mapped
 *     non-cacheable, so the uncached 0xC0000000 alias is coherent.
 *   - Pi 1 (ARMv6, BCM2835): the ARM goes through the VideoCore L2 cache, so
 *     DMA must use the L2-COHERENT 0x40000000 alias. The uncached 0xC0000000
 *     alias bypasses L2 and reads/writes stale data (intermittent garbage). */
#if defined(__aarch64__)
#  define USB_BUS_ADDR(p)         (((uint32_t)(uintptr_t)(p)) | 0xC0000000u)
#else
#  define USB_BUS_ADDR(p)         (((uint32_t)(uintptr_t)(p)) | 0x40000000u)
#endif

/* Data Synchronisation Barrier, portable across AArch64 (Pi 3/400) and
 * ARMv6 (Pi 1, ARM1176): same ordering guarantee, different encoding. */
#if defined(__aarch64__)
#  define USB_DSB() __asm__ volatile("dsb sy" ::: "memory")
#else
#  define USB_DSB() __asm__ volatile("mcr p15, 0, %0, c7, c10, 4" :: "r"(0) : "memory")
#endif

/* ---- MMIO accessors (DWC2 is Device memory: strongly ordered) ---- */
static inline uint32_t usb_rd(uint32_t off)
{
    return *(volatile uint32_t *)(USB_BASE + off);
}
static inline void usb_wr(uint32_t off, uint32_t val)
{
    *(volatile uint32_t *)(USB_BASE + off) = val;
    USB_DSB();
}

/* Power the USB controller (VideoCore mailbox) and report the core identity
 * over UART. Returns 0 if the DWC2 core answers with a valid Synopsys ID,
 * -1 otherwise (or on a non-Pi3 board, where this path is a no-op). */
int usb_dwc2_probe(void);

/* Step 2: soft-reset the core, select the internal UTMI+ HS PHY, force host
 * mode, enable internal DMA, and size the FIFOs. Leaves the core in host mode
 * with the root port not yet powered (that is Step 3). Returns 0 on success. */
int usb_host_init(void);

/* Step 3: power & reset the root port, detect the connected device's speed.
 * Returns 0 with the port enabled, -1 on no-connect or enable failure. */
int usb_port_init(void);

/* Step 4: issue the first control transfer (GET_DESCRIPTOR) on EP0 and read the
 * device descriptor of the device at address 0 (the LAN9514 hub). Returns 0. */
int usb_enum_probe(void);

/* Step 5: SET_ADDRESS + SET_CONFIGURATION the hub and read its port count. */
int usb_hub_init(void);

/* Step 6: power the hub's ports, find the connected one, and reset it. */
int usb_hub_scan(void);

/* Step 7: address + configure the Ethernet device, map its endpoints. */
int usb_eth_enum(void);

/* Step 8: prove SMSC9512 register access (ID, lite reset, MAC read). */
int usb_eth_chip_probe(void);

/* Step 9: program MAC, bring up the PHY (MII), enable TX/RX. */
int usb_eth_chip_init(void);

/* Step 10: wait for link, set duplex, send a frame and receive frames. */
int usb_eth_link_test(void);

/* Step 11: the raw-frame NIC API the Mongoose stack drives (via genet_net_*).
 *   usbnet_init: full bring-up, idempotent, returns 100 or -1.
 *   usbnet_tx/rx: one frame (no FCS).  usbnet_up: link state.  */
int           usbnet_init(void);
unsigned long usbnet_tx(const void *frame, unsigned long len);
unsigned long usbnet_rx(void *buf, unsigned long max);
int           usbnet_up(void);
void          usbnet_get_mac(unsigned char mac[6]);

#endif /* USB_DWC2_H */