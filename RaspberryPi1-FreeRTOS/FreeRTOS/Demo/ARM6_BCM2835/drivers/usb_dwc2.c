/* DWC2 USB host bring-up for the Pi 3 (BCM2837) -- Step 1: power + identify.
 *
 * This step does the smallest thing that proves the controller is reachable
 * and alive before any of the hard work begins:
 *   1. Ask the VideoCore firmware to power the USB HCD (mailbox set-power-state).
 *   2. Read the Synopsys core ID and the four GHWCFG capability registers.
 *   3. Decode the bits later steps depend on (DMA architecture, host channel
 *      count, HS PHY type, FIFO depth) and confirm the AHB master is idle.
 *
 * Nothing is written to the core yet -- a soft reset and host init come in
 * Step 2 -- so this is the lowest-risk way to confirm power and the register
 * window on real hardware.
 */

#include <stdint.h>
#include "usb_dwc2.h"
#include "uart.h"
#include "timer.h"   /* sys_us(), delay_us() */

#if (PERIPHERAL_BASE == 0xFE000000UL)
/* ---- BCM2711 (Pi 4 / Pi 400): these boards use GENET, not USB Ethernet ---- */
int usb_dwc2_probe(void) { uart_puts("USB: Pi 2/3 path only\r\n"); return -1; }
int usb_host_init(void)  { return -1; }
int usb_port_init(void)  { return -1; }
int usb_enum_probe(void) { return -1; }
int usb_hub_init(void)   { return -1; }
int usb_hub_scan(void)   { return -1; }
int usb_eth_enum(void)   { return -1; }
int usb_eth_chip_probe(void) { return -1; }
int usb_eth_chip_init(void)  { return -1; }
int usb_eth_link_test(void)  { return -1; }
int usbnet_init(void) { return -1; }
unsigned long usbnet_tx(const void *s, unsigned long l) { (void)s; (void)l; return 0; }
unsigned long usbnet_rx(void *d, unsigned long m) { (void)d; (void)m; return 0; }
int usbnet_up(void) { return 0; }
void usbnet_get_mac(unsigned char mac[6]) { for (int i=0;i<6;i++) mac[i]=0; }

#else
/* ======================= BCM2837 (Pi 2 / Pi 3) ========================== */

/* ---- tiny UART number helpers (same style as genet.c) ---- */
static void put_hex32(uint32_t v)
{
    uart_puts("0x");
    for (int s = 28; s >= 0; s -= 4) {
        uint32_t n = (v >> s) & 0xF;
        uart_putc(n < 10 ? (char)('0' + n) : (char)('A' + n - 10));
    }
}
static void put_dec(uint32_t v)
{
    char b[12]; int i = 11; b[i] = 0;
    if (!v) { uart_puts("0"); return; }
    while (v) { b[--i] = (char)('0' + v % 10); v /= 10; }
    uart_puts(&b[i]);
}

/* ---- VideoCore property mailbox (self-contained; channel 8) ----
 * 16-byte-aligned buffer; the low 4 bits of the address carry the channel. */
#define MBOX_BASE_   (PERIPHERAL_BASE + 0xB880u)
#define MBOX_READ_   (*(volatile uint32_t *)(MBOX_BASE_ + 0x00u))
#define MBOX_STATUS_ (*(volatile uint32_t *)(MBOX_BASE_ + 0x18u))
#define MBOX_WRITE_  (*(volatile uint32_t *)(MBOX_BASE_ + 0x20u))
#define MBOX_FULL_   0x80000000u
#define MBOX_EMPTY_  0x40000000u

static volatile uint32_t __attribute__((aligned(16))) s_mbox[36];

static int mbox_call(uint32_t ch)
{
    uint32_t addr = ((uint32_t)(uintptr_t)s_mbox & ~0xFu) | (ch & 0xFu);
    while (MBOX_STATUS_ & MBOX_FULL_) {}
    USB_DSB();
    MBOX_WRITE_ = addr;
    for (;;) {
        while (MBOX_STATUS_ & MBOX_EMPTY_) {}
        USB_DSB();
        if (MBOX_READ_ == addr) return s_mbox[1] == 0x80000000u;
    }
}

/* Set power state for a device id (mailbox tag 0x00028001).
 * state bit0 = power on, bit1 = wait for stable. Returns the response state. */
#define POWER_DEVICE_USB_HCD  0x00000003u
static uint32_t mbox_set_power(uint32_t device_id, uint32_t state)
{
    int i = 0;
    s_mbox[i++] = 0;                 /* total size (patched) */
    s_mbox[i++] = 0;                 /* request              */
    s_mbox[i++] = 0x00028001u;       /* set power state      */
    s_mbox[i++] = 8;                 /* value buffer bytes   */
    s_mbox[i++] = 0;                 /* req/resp code        */
    s_mbox[i++] = device_id;
    s_mbox[i++] = state;
    s_mbox[i++] = 0;                 /* end tag              */
    s_mbox[0]   = (uint32_t)(i * 4);
    if (!mbox_call(8)) return 0xFFFFFFFFu;
    return s_mbox[6];                /* returned state */
}

int usb_dwc2_probe(void)
{
    /* 1. Power the USB host controller (on + wait for stable). */
    uint32_t st = mbox_set_power(POWER_DEVICE_USB_HCD, 0x3u);
    uart_puts("USB: power state -> ");
    if (st == 0xFFFFFFFFu)      uart_puts("mailbox FAILED\r\n");
    else if (st & 0x1u)         uart_puts("ON\r\n");
    else                        uart_puts("OFF (device missing?)\r\n");

    /* 2. Identify the core. */
    uint32_t id  = usb_rd(GSNPSID);
    uint32_t hw1 = usb_rd(GHWCFG1);
    uint32_t hw2 = usb_rd(GHWCFG2);
    uint32_t hw3 = usb_rd(GHWCFG3);
    uint32_t hw4 = usb_rd(GHWCFG4);
    uint32_t rst = usb_rd(GRSTCTL);

    uart_puts("USB: GSNPSID="); put_hex32(id);
    if ((id >> 16) == 0x4F54u) {                 /* "OT" => valid DWC_otg core */
        uint32_t rev = id & 0xFFFFu;             /* BCD-ish version, e.g. 0x280A */
        uart_puts("  OTG v"); put_hex32(rev);
    }
    uart_puts("\r\n");

    /* GHWCFG2: arch[4:3], hs-phy[7:6], host-channels[17:14] */
    uint32_t arch  = (hw2 >> 3)  & 0x3u;
    uint32_t hsphy = (hw2 >> 6)  & 0x3u;
    uint32_t nch   = ((hw2 >> 14) & 0xFu) + 1u;
    uart_puts("USB: GHWCFG2="); put_hex32(hw2);
    uart_puts("  arch=");
    uart_puts(arch == 2u ? "internal-DMA" : arch == 1u ? "external-DMA" : "slave-only");
    uart_puts(" hsphy=");
    uart_puts(hsphy == 1u ? "UTMI+" : hsphy == 2u ? "ULPI" : hsphy == 3u ? "UTMI+ULPI" : "none");
    uart_puts(" host_ch="); put_dec(nch);
    uart_puts("\r\n");

    uint32_t dfifo = (hw3 >> 16) & 0xFFFFu;       /* total DFIFO words */
    uart_puts("USB: GHWCFG3="); put_hex32(hw3);
    uart_puts("  dfifo_words="); put_dec(dfifo); uart_puts("\r\n");

    uart_puts("USB: GHWCFG1="); put_hex32(hw1);
    uart_puts("  GHWCFG4="); put_hex32(hw4); uart_puts("\r\n");

    uart_puts("USB: GRSTCTL="); put_hex32(rst);
    uart_puts("  AHB_idle="); uart_puts((rst & GRSTCTL_AHBIDLE) ? "1" : "0");
    uart_puts("\r\n");

    if ((id >> 16) == 0x4F54u) {
        uart_puts("USB: DWC2 core alive\r\n");
        return 0;
    }
    uart_puts("USB: DWC2 not responding (bad ID)\r\n");
    return -1;
}

/* ---- FIFO layout (32-bit words). Total must be <= GHWCFG3 dfifo (4080). ---- */
#define RX_FIFO_WORDS    1024u
#define NPTX_FIFO_WORDS  1024u
#define PTX_FIFO_WORDS   1024u        /* 3072 total, fits the 4080-word core */

/* Wait for the AHB master to go idle (bounded). */
static int wait_ahb_idle(void)
{
    uint64_t t0 = sys_us();
    while (!(usb_rd(GRSTCTL) & GRSTCTL_AHBIDLE))
        if (sys_us() - t0 > 100000ULL) return -1;
    return 0;
}

/* Core soft reset: assert CSFTRST, wait for self-clear, then AHB idle. */
static int core_soft_reset(void)
{
    if (wait_ahb_idle() < 0) return -1;
    usb_wr(GRSTCTL, usb_rd(GRSTCTL) | GRSTCTL_CSFTRST);
    uint64_t t0 = sys_us();
    while (usb_rd(GRSTCTL) & GRSTCTL_CSFTRST)
        if (sys_us() - t0 > 100000ULL) return -1;
    delay_us(100000);     /* PHY clock must stabilise (~100ms) before any further
                             register access, or the AHB read stalls the CPU */
    return wait_ahb_idle();
}

static void fifo_flush_tx_all(void)
{
    usb_wr(GRSTCTL, GRSTCTL_TXFFLSH | (0x10u << GRSTCTL_TXFNUM_SHIFT)); /* 0x10 = all */
    uint64_t t0 = sys_us();
    while (usb_rd(GRSTCTL) & GRSTCTL_TXFFLSH)
        if (sys_us() - t0 > 10000ULL) break;
}
static void fifo_flush_rx(void)
{
    usb_wr(GRSTCTL, GRSTCTL_RXFFLSH);
    uint64_t t0 = sys_us();
    while (usb_rd(GRSTCTL) & GRSTCTL_RXFFLSH)
        if (sys_us() - t0 > 10000ULL) break;
}

int usb_host_init(void)
{
    uart_puts("USB: host core init...\r\n");

    /* Make sure the core isn't clock-gated. */
    usb_wr(PCGCCTL, 0);                                    uart_puts("  .pcgc\r\n");

    /* Mask the global interrupt line while we reconfigure. */
    usb_wr(GAHBCFG, usb_rd(GAHBCFG) & ~GAHBCFG_GLBLINTRMSK); uart_puts("  .mask\r\n");

    /* Pre-reset PHY housekeeping (Circle order): clear external-VBUS-drive and
     * the TERM_SEL delay pulse before the soft reset. */
    uint32_t cfg = usb_rd(GUSBCFG);
    cfg &= ~GUSBCFG_ULPI_EXT_VBUS_DRV;
    cfg &= ~GUSBCFG_TERM_SEL_DL_PULSE;
    usb_wr(GUSBCFG, cfg);                                  uart_puts("  .precfg\r\n");

    /* Soft reset the whole core. */
    if (core_soft_reset() < 0) { uart_puts("USB: core reset TIMEOUT\r\n"); return -1; }
    uart_puts("  .reset\r\n");

    /* Select the internal high-speed UTMI+ PHY, 8-bit. */
    cfg = usb_rd(GUSBCFG);
    cfg &= ~GUSBCFG_ULPI_UTMI_SEL;     /* UTMI+ (not ULPI) */
    cfg &= ~GUSBCFG_PHYIF;             /* 8-bit UTMI       */
    usb_wr(GUSBCFG, cfg);                                  uart_puts("  .phy\r\n");

    /* Enable the core's internal DMA engine (coherent thanks to NC RAM). */
    usb_wr(GAHBCFG, usb_rd(GAHBCFG) | GAHBCFG_DMAEN);      uart_puts("  .dma\r\n");

    /* Disable HNP/SRP and force host mode (deterministic for a host-only role). */
    cfg = usb_rd(GUSBCFG);
    cfg &= ~(GUSBCFG_HNPCAP | GUSBCFG_SRPCAP);
    cfg |=  GUSBCFG_FHMOD;
    cfg &= ~GUSBCFG_FDMOD;
    usb_wr(GUSBCFG, cfg);
    delay_us(50000);                   /* spec: >=25ms after forcing mode */
    uart_puts("  .host\r\n");

    /* Host clock: 30/60MHz PHY clock, allow high speed. */
    uint32_t hcfg = usb_rd(HCFG);
    hcfg &= ~HCFG_FSLSPCLKSEL_MASK;    /* 0 = 30/60MHz for UTMI+ */
    hcfg &= ~HCFG_FSLSSUPP;            /* don't restrict to FS/LS */
    usb_wr(HCFG, hcfg);                                    uart_puts("  .hcfg\r\n");

    /* Carve the data FIFO: Rx | non-periodic Tx | periodic Tx. */
    usb_wr(GRXFSIZ,   RX_FIFO_WORDS);
    usb_wr(GNPTXFSIZ, (NPTX_FIFO_WORDS << 16) | RX_FIFO_WORDS);
    usb_wr(HPTXFSIZ,  (PTX_FIFO_WORDS  << 16) | (RX_FIFO_WORDS + NPTX_FIFO_WORDS));
    uart_puts("  .fifo\r\n");

    /* Flush the freshly-sized FIFOs. */
    fifo_flush_tx_all();
    fifo_flush_rx();                                       uart_puts("  .flush\r\n");

    /* Clear any pending core interrupts. */
    usb_wr(GINTSTS, 0xFFFFFFFFu);

    /* ---- confirm ---- */
    uint32_t gintsts = usb_rd(GINTSTS);
    uart_puts("USB: CURMOD=");
    uart_puts((gintsts & GINTSTS_CURMOD_HOST) ? "host" : "device (!)");
    uart_puts("\r\n");
    uart_puts("USB: GUSBCFG="); put_hex32(usb_rd(GUSBCFG));
    uart_puts(" GAHBCFG=");     put_hex32(usb_rd(GAHBCFG));
    uart_puts(" HCFG=");        put_hex32(usb_rd(HCFG));
    uart_puts("\r\n");
    uart_puts("USB: FIFO rx="); put_dec(RX_FIFO_WORDS);
    uart_puts(" nptx=");        put_dec(NPTX_FIFO_WORDS);
    uart_puts(" ptx=");         put_dec(PTX_FIFO_WORDS);
    uart_puts("\r\n");
    uart_puts("USB: HPRT0=");   put_hex32(usb_rd(HPRT0)); uart_puts("\r\n");

    if (!(gintsts & GINTSTS_CURMOD_HOST)) {
        uart_puts("USB: NOT in host mode\r\n");
        return -1;
    }
    uart_puts("USB: host core ready\r\n");
    return 0;
}

/* HPRT0 read for a safe read-modify-write: drop the write-1-to-clear / disable
 * bits so a plain write neither clears a change flag nor disables the port. */
static uint32_t hprt_rmw(void)
{
    return usb_rd(HPRT0) & ~HPRT0_WC_BITS;
}

/* Step 3: power the root port, wait for the onboard device (LAN9514) to
 * connect, drive a USB reset, and read the negotiated speed. The HS reset/
 * enable handshake is timing-sensitive, so we poll for enable (rather than a
 * fixed delay) and retry the reset a few times -- some devices/hubs need a
 * second reset before the port enables. Returns 0 with the port enabled. */
int usb_port_init(void)
{
    uart_puts("USB: root port power on\r\n");

    uint32_t hprt = hprt_rmw();
    if (!(hprt & HPRT0_PWR)) usb_wr(HPRT0, hprt | HPRT0_PWR);
    delay_us(100000);                              /* power + inrush settle */

    for (int attempt = 1; attempt <= 3; attempt++) {
        /* Wait for the device to assert connect. */
        uint64_t t0 = sys_us();
        while (!(usb_rd(HPRT0) & HPRT0_CONNSTS)) {
            if (sys_us() - t0 > 1000000ULL) {
                uart_puts("USB: no device on root port  HPRT0=");
                put_hex32(usb_rd(HPRT0)); uart_puts("\r\n");
                return -1;
            }
        }
        uart_puts("USB: device connected\r\n");
        delay_us(100000);                          /* connect debounce (~100ms) */
        usb_wr(HPRT0, hprt_rmw() | HPRT0_CONNDET); /* ack connect change */

        uart_puts("USB: bus reset");
        if (attempt > 1) { uart_puts(" (retry "); put_dec((uint32_t)attempt); uart_puts(")"); }
        uart_puts("...\r\n");

        /* Drive the reset: assert >=50ms, then release. */
        usb_wr(HPRT0, hprt_rmw() | HPRT0_RST);
        delay_us(60000);
        usb_wr(HPRT0, hprt_rmw() & ~HPRT0_RST);

        /* Poll for the port to enable (up to ~120ms). A PRTENCHNG with the
         * enable bit clear means the enable failed -> break out and retry. */
        int enabled = 0;
        t0 = sys_us();
        while (sys_us() - t0 < 120000ULL) {
            hprt = usb_rd(HPRT0);
            if (hprt & HPRT0_ENA) { enabled = 1; break; }
            if ((hprt & HPRT0_ENCHNG) && !(hprt & HPRT0_ENA)) break;
        }
        usb_wr(HPRT0, hprt_rmw() | HPRT0_ENCHNG | HPRT0_CONNDET);   /* ack changes */

        if (enabled) {
            uint32_t spd = (hprt & HPRT0_SPD_MASK) >> HPRT0_SPD_SHIFT;
            uart_puts("USB: HPRT0="); put_hex32(hprt); uart_puts("\r\n");
            uart_puts("USB: port enabled, speed=");
            uart_puts(spd == 0u ? "HIGH" : spd == 1u ? "FULL" : spd == 2u ? "LOW" : "?");
            uart_puts("\r\n");
            uart_puts("USB: ===== ROOT PORT READY =====\r\n");
                 /* let the result sit before the task flood */
            return 0;
        }
        uart_puts("USB: enable failed, retrying\r\n");
        delay_us(50000);
    }
    uart_puts("USB: ===== PORT FAILED TO ENABLE =====\r\n");
    
    return -1;
}

/* ===================== Step 4: EP0 control transfers ===================== */

/* USB standard setup packet (8 bytes, little-endian on the wire). */
struct usb_setup {
    uint8_t  bmRequestType;
    uint8_t  bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} __attribute__((packed));

/* DMA buffers live in (non-cacheable) RAM, aligned for the channel DMA. */
static struct usb_setup __attribute__((aligned(64))) s_setup;
static uint8_t          __attribute__((aligned(64))) s_descbuf[256];

static void put_hex16(uint32_t v)
{
    uart_puts("0x");
    for (int sft = 12; sft >= 0; sft -= 4) {
        uint32_t n = (v >> sft) & 0xF;
        uart_putc(n < 10 ? (char)('0' + n) : (char)('A' + n - 10));
    }
}

/* Cleanly stop a host channel: if it is enabled, request disable and wait for
 * it to actually go down, then clear its interrupts. This is what keeps a
 * timed-out or wedged transfer from poisoning every transfer that follows. */
static void chan_disable(int ch)
{
    uint32_t base = HC_BASE(ch);
    uint32_t cc = usb_rd(base + HCCHAR);
    if (cc & HCCHAR_CHENA) {
        usb_wr(base + HCCHAR, cc | HCCHAR_CHDIS | HCCHAR_CHENA);
        uint64_t t0 = sys_us();
        while ((usb_rd(base + HCCHAR) & HCCHAR_CHENA) && sys_us() - t0 < 50000ULL) { }
    }
    usb_wr(base + HCINT, 0xFFFFFFFFu);
}

/* Run one transaction on host channel `ch` in internal-DMA mode and wait
 * (polled) for it to halt. Retries NAK/NYET/transaction-errors. Returns 0 on
 * XFERCOMPL, -1 STALL, -2 timeout, -3 too many retries. */
#define CH_RETRIES 50
static int chan_xfer(int ch, uint8_t dev_addr, uint8_t ep, int dir_in, uint32_t ep_type,
                     uint32_t mps, uint32_t pid, void *buf, int len)
{
    uint32_t base = HC_BASE(ch);
    int pktcnt = (len + (int)mps - 1) / (int)mps;
    if (pktcnt < 1) pktcnt = 1;                       /* zero-length = 1 packet */

    chan_disable(ch);                                 /* always start from a clean channel */
    for (int attempt = 0; attempt < CH_RETRIES; attempt++) {
        usb_wr(base + HCINT, 0xFFFFFFFFu);            /* clear stale flags     */
        usb_wr(base + HCINTMSK, 0);                   /* poll, no interrupts   */
        usb_wr(base + HCTSIZ,
               ((pid & 0x3u) << HCTSIZ_PID_SHIFT) |
               (((uint32_t)pktcnt & 0x3FFu) << HCTSIZ_PKTCNT_SHIFT) |
               ((uint32_t)len & HCTSIZ_XFERSIZE_MASK));
        usb_wr(base + HCDMA, buf ? USB_BUS_ADDR(buf) : 0);
        usb_wr(base + HCSPLT, 0);                      /* HS device, no split   */

        uint32_t hcchar =
              (mps & HCCHAR_MPS_MASK)
            | (((uint32_t)ep & 0xF) << HCCHAR_EPNUM_SHIFT)
            | (dir_in ? HCCHAR_EPDIR_IN : 0u)
            | ((ep_type & 0x3u) << HCCHAR_EPTYPE_SHIFT)
            | (1u << HCCHAR_MC_SHIFT)
            | (((uint32_t)dev_addr & 0x7Fu) << HCCHAR_DEVADDR_SHIFT)
            | HCCHAR_CHENA;
        usb_wr(base + HCCHAR, hcchar);                /* go */

        /* Wait for the channel to halt. */
        uint64_t t0 = sys_us();
        uint32_t hcint;
        for (;;) {
            hcint = usb_rd(base + HCINT);
            if (hcint & HCINT_CHHLTD) break;
            if (sys_us() - t0 > 1000000ULL) { chan_disable(ch); return -2; }
        }
        USB_DSB();   /* drain DWC2 DMA writes before the CPU reads the buffer
                        (ARMv6 has no implicit ordering here; stale reads otherwise) */
        usb_wr(base + HCINT, 0xFFFFFFFFu);
        if (hcint & HCINT_XFERCOMPL) return 0;
        if (hcint & HCINT_STALL)     return -1;
        /* NAK / NYET / transaction error -> back off and retry. */
        delay_us(2000);
    }
    return -3;
}

/* A full control transfer: SETUP, optional DATA, then the zero-length STATUS
 * in the opposite direction. ep0_mps is the EP0 max packet size (64 for HS). */
static int control_xfer(uint8_t dev_addr, uint32_t ep0_mps,
                        const struct usb_setup *setup, void *data, int data_len)
{
    s_setup = *setup;                                   /* into the DMA buffer */

    /* SETUP stage: 8-byte OUT with a SETUP PID. */
    if (chan_xfer(0, dev_addr, 0, 0, EPTYPE_CONTROL, ep0_mps, USB_PID_SETUP, &s_setup, 8) != 0)
        return -1;

    int dir_in = (setup->bmRequestType & 0x80u) != 0;

    /* DATA stage (control data always starts on DATA1). */
    if (data_len > 0) {
        if (chan_xfer(0, dev_addr, 0, dir_in, EPTYPE_CONTROL, ep0_mps,
                      USB_PID_DATA1, data, data_len) != 0)
            return -2;
    }

    /* STATUS stage: opposite direction, zero length, DATA1. */
    int status_in = (data_len > 0) ? !dir_in : 1;
    if (chan_xfer(0, dev_addr, 0, status_in, EPTYPE_CONTROL, ep0_mps,
                  USB_PID_DATA1, 0, 0) != 0)
        return -3;
    return 0;
}

int usb_enum_probe(void)
{
    if (!(usb_rd(HPRT0) & HPRT0_ENA)) {
        uart_puts("USB: port not enabled, skipping enum\r\n");
        return -1;
    }
    uart_puts("USB: GET_DESCRIPTOR(device) addr 0 ...\r\n");

    struct usb_setup s = {
        .bmRequestType = 0x80,        /* IN | standard | device */
        .bRequest      = 0x06,        /* GET_DESCRIPTOR         */
        .wValue        = 0x0100,      /* type=1 (device), idx 0 */
        .wIndex        = 0,
        .wLength       = 18,
    };

    int r = control_xfer(0, 64, &s, s_descbuf, 18);
    if (r != 0) {
        uart_puts("USB: descriptor read FAILED, r="); put_dec((uint32_t)(-r)); uart_puts("\r\n");
        
        return -1;
    }

    uint8_t  blen = s_descbuf[0];
    uint8_t  btype = s_descbuf[1];
    uint8_t  bclass = s_descbuf[4];
    uint8_t  mps0  = s_descbuf[7];
    uint32_t vid = (uint32_t)s_descbuf[8]  | ((uint32_t)s_descbuf[9]  << 8);
    uint32_t pid = (uint32_t)s_descbuf[10] | ((uint32_t)s_descbuf[11] << 8);

    uart_puts("USB: desc len=");  put_dec(blen);
    uart_puts(" type=");          put_dec(btype);
    uart_puts(" class=");         put_dec(bclass);
    uart_puts(" bMaxPacketSize0="); put_dec(mps0);
    uart_puts("\r\n");
    uart_puts("USB: idVendor=");  put_hex16(vid);
    uart_puts(" idProduct=");     put_hex16(pid);
    uart_puts("\r\n");
    if (vid == 0x0424u)
        uart_puts("USB: ===== SMSC/Microchip hub on EP0 (control transfers work!) =====\r\n");
    else
        uart_puts("USB: ===== EP0 control transfer OK =====\r\n");
    
    return 0;
}

/* ============= Step 5: address + configure the hub, count ports ========= */

static uint8_t g_hub_addr;     /* USB address assigned to the LAN9514 hub */
static uint8_t g_hub_ports;    /* number of downstream ports it reports   */
static uint8_t g_hub_pwr2good; /* bPwrOn2PwrGood from hub descriptor (x2ms)*/

/* ---- standard-request helpers (all on EP0, channel 0) ---- */
static int get_descriptor(uint8_t addr, uint32_t mps, uint8_t type, uint8_t index,
                          uint16_t lang, void *buf, int len)
{
    struct usb_setup s = {
        .bmRequestType = 0x80,                       /* IN | standard | device */
        .bRequest      = 0x06,                       /* GET_DESCRIPTOR         */
        .wValue        = (uint16_t)(((uint16_t)type << 8) | index),
        .wIndex        = lang,
        .wLength       = (uint16_t)len,
    };
    return control_xfer(addr, mps, &s, buf, len);
}

static int set_address(uint8_t new_addr)             /* sent to address 0 */
{
    struct usb_setup s = {
        .bmRequestType = 0x00, .bRequest = 0x05,     /* SET_ADDRESS */
        .wValue = new_addr, .wIndex = 0, .wLength = 0,
    };
    return control_xfer(0, 64, &s, 0, 0);
}

static int set_configuration(uint8_t addr, uint8_t cfg)
{
    struct usb_setup s = {
        .bmRequestType = 0x00, .bRequest = 0x09,     /* SET_CONFIGURATION */
        .wValue = cfg, .wIndex = 0, .wLength = 0,
    };
    return control_xfer(addr, 64, &s, 0, 0);
}

static int get_hub_descriptor(uint8_t addr, void *buf, int len)
{
    struct usb_setup s = {
        .bmRequestType = 0xA0, .bRequest = 0x06,     /* IN | class | device */
        .wValue = (uint16_t)(0x29u << 8),            /* type 0x29 = HUB */
        .wIndex = 0, .wLength = (uint16_t)len,
    };
    return control_xfer(addr, 64, &s, buf, len);
}

int usb_hub_init(void)
{
    if (!(usb_rd(HPRT0) & HPRT0_ENA)) {
        uart_puts("USB: port not enabled, skipping hub init\r\n");
        return -1;
    }

    /* 1. Give the hub address 1. */
    uart_puts("USB: SET_ADDRESS(1) for hub...\r\n");
    if (set_address(1) != 0) { uart_puts("USB: SET_ADDRESS failed\r\n");  return -1; }
    g_hub_addr = 1;
    delay_us(5000);                                   /* SET_ADDRESS recovery (>2ms) */

    /* 2. Confirm by re-reading the device descriptor at the new address. */
    if (get_descriptor(g_hub_addr, 64, 1, 0, 0, s_descbuf, 18) != 0) {
        uart_puts("USB: descriptor re-read at addr 1 FAILED\r\n");  return -1;
    }
    uint32_t vid = (uint32_t)s_descbuf[8] | ((uint32_t)s_descbuf[9] << 8);
    uart_puts("USB: hub now at addr 1, idVendor="); put_hex16(vid); uart_puts("\r\n");

    /* 3. Read the configuration descriptor header (9 bytes). */
    if (get_descriptor(g_hub_addr, 64, 2, 0, 0, s_descbuf, 9) != 0) {
        uart_puts("USB: config descriptor read FAILED\r\n");  return -1;
    }
    uint8_t  cfgval = s_descbuf[5];
    uint32_t totlen = (uint32_t)s_descbuf[2] | ((uint32_t)s_descbuf[3] << 8);
    uart_puts("USB: config value="); put_dec(cfgval);
    uart_puts(" totalLen=");          put_dec(totlen); uart_puts("\r\n");

    /* 4. Select that configuration. */
    if (set_configuration(g_hub_addr, cfgval) != 0) {
        uart_puts("USB: SET_CONFIGURATION failed\r\n");  return -1;
    }
    uart_puts("USB: hub configured\r\n");

    /* 5. Hub class descriptor -> downstream port count. */
    if (get_hub_descriptor(g_hub_addr, s_descbuf, 8) != 0) {
        uart_puts("USB: hub descriptor read FAILED\r\n");  return -1;
    }
    g_hub_ports    = s_descbuf[2];                     /* bNbrPorts */
    g_hub_pwr2good = s_descbuf[5];                      /* bPwrOn2PwrGood (x2ms) */
    uint16_t hubchar = (uint16_t)(s_descbuf[3] | (s_descbuf[4] << 8));
    uart_puts("USB: hubChar="); put_hex16(hubchar);
    uart_puts(" pwr2good=");    put_dec((uint32_t)g_hub_pwr2good * 2); uart_puts("ms\r\n");
    uart_puts("USB: ===== HUB READY, ports=");
    put_dec(g_hub_ports);
    uart_puts(" =====\r\n");
    
    return 0;
}

/* ============= Step 6: power hub ports, find + reset the device =========== */

/* Hub class feature selectors (USB 2.0 §11.24.2). */
#define HUBF_PORT_CONNECTION    0u
#define HUBF_PORT_ENABLE        1u
#define HUBF_PORT_RESET         4u
#define HUBF_PORT_POWER         8u
#define HUBF_C_PORT_CONNECTION  16u
#define HUBF_C_PORT_RESET       20u

/* wPortStatus bits. */
#define PS_CONNECTION           0x0001u
#define PS_ENABLE               0x0002u
#define PS_RESET                0x0010u
#define PS_LOWSPEED             0x0200u
#define PS_HIGHSPEED            0x0400u

static uint8_t g_dev_port;      /* hub port that carries the (Ethernet) device */

static int hub_set_feature(uint8_t addr, uint16_t feat, uint16_t port)
{
    struct usb_setup s = { .bmRequestType = 0x23, .bRequest = 0x03,   /* SET_FEATURE, other */
                           .wValue = feat, .wIndex = port, .wLength = 0 };
    return control_xfer(addr, 64, &s, 0, 0);
}
static int hub_clear_feature(uint8_t addr, uint16_t feat, uint16_t port)
{
    struct usb_setup s = { .bmRequestType = 0x23, .bRequest = 0x01,   /* CLEAR_FEATURE, other */
                           .wValue = feat, .wIndex = port, .wLength = 0 };
    return control_xfer(addr, 64, &s, 0, 0);
}
static int hub_port_status(uint8_t addr, uint16_t port, uint16_t *st)
{
    struct usb_setup s = { .bmRequestType = 0xA3, .bRequest = 0x00,   /* GET_STATUS, other */
                           .wValue = 0, .wIndex = port, .wLength = 4 };
    int r = control_xfer(addr, 64, &s, s_descbuf, 4);
    if (r == 0 && st) *st = (uint16_t)(s_descbuf[0] | (s_descbuf[1] << 8));
    return r;
}

int usb_hub_scan(void)
{
    if (g_hub_ports == 0) { uart_puts("USB: hub not ready, skip scan\r\n"); return -1; }

    /* Power every downstream port. */
    uart_puts("USB: powering hub ports...\r\n");
    for (uint16_t p = 1; p <= g_hub_ports; p++) hub_set_feature(g_hub_addr, HUBF_PORT_POWER, p);

    /* Honor the hub's own power-on-to-power-good time (x2ms), floor 100ms. */
    uint32_t settle = (uint32_t)g_hub_pwr2good * 2000u;
    if (settle < 100000u) settle = 100000u;
    delay_us(settle);

    /* Poll for the first connected downstream port for up to ~2s. The LAN9514's
       internal Ethernet bridge can assert connect later than a single read. */
    int      found = 0;
    uint16_t conn_p = 0, st = 0;
    uint64_t t0 = sys_us();
    int swept = 0;
    while (conn_p == 0 && (uint32_t)(sys_us() - t0) < 2000000u) {
        for (uint16_t p = 1; p <= g_hub_ports; p++) {
            uint16_t s = 0;
            if (hub_port_status(g_hub_addr, p, &s) != 0) continue;
            if (!swept) { uart_puts("USB: port "); put_dec(p);
                          uart_puts(" status="); put_hex16(s); uart_puts("\r\n"); }
            if (s & PS_CONNECTION) { conn_p = p; st = s; break; }
        }
        swept = 1;                                     /* print the sweep once */
        if (conn_p == 0) delay_us(100000);
    }

    if (conn_p != 0) {
        uint16_t p = conn_p;
        uart_puts("USB: port "); put_dec(p); uart_puts(" CONNECTED status="); put_hex16(st); uart_puts("\r\n");

        /* Debounce, then reset the port. */
        hub_clear_feature(g_hub_addr, HUBF_C_PORT_CONNECTION, p);
        delay_us(100000);
        hub_set_feature(g_hub_addr, HUBF_PORT_RESET, p);

        /* Wait for the hub to finish the reset (RESET bit self-clears). */
        for (int i = 0; i < 30; i++) {
            delay_us(10000);
            if (hub_port_status(g_hub_addr, p, &st) != 0) continue;
            if (!(st & PS_RESET)) break;
        }
        hub_clear_feature(g_hub_addr, HUBF_C_PORT_RESET, p);
        delay_us(20000);                               /* reset recovery */

        const char *spd = (st & PS_HIGHSPEED) ? "HIGH" : (st & PS_LOWSPEED) ? "LOW" : "FULL";
        uart_puts("USB: port "); put_dec(p); uart_puts(" reset -> status="); put_hex16(st);
        uart_puts(" speed="); uart_puts(spd); uart_puts("\r\n");

        if (st & PS_ENABLE) { g_dev_port = (uint8_t)p; found = 1; }
    }

    if (found) {
        uart_puts("USB: ===== DEVICE ON PORT "); put_dec(g_dev_port);
        uart_puts(" READY (addr 0) =====\r\n");
    } else {
        uart_puts("USB: no connected downstream device (USB-A empty? eth present?)\r\n");
    }
    
    return found ? 0 : -1;
}

/* ============= Step 7: enumerate the SMSC9512 Ethernet device ============ */

/* Discovered device facts the NIC driver (next steps) will need. */
static uint8_t  g_eth_addr;          /* USB address assigned to the MAC      */
static uint8_t  g_eth_bulk_in;       /* bulk IN endpoint number (frames in)  */
static uint8_t  g_eth_bulk_out;      /* bulk OUT endpoint number (frames out)*/
static uint8_t  g_eth_intr_in;       /* interrupt IN endpoint (link status)  */
static uint16_t g_eth_in_mps;        /* bulk IN max packet size  (512 @ HS)  */
static uint16_t g_eth_out_mps;       /* bulk OUT max packet size (512 @ HS)  */

int usb_eth_enum(void)
{
    if (g_dev_port == 0) { uart_puts("USB: no device port, skip eth enum\r\n"); return -1; }

    /* 1. Address the device (it is alone at address 0 behind hub port 1). */
    uart_puts("USB: SET_ADDRESS(2) for device...\r\n");
    if (set_address(2) != 0) { uart_puts("USB: device SET_ADDRESS failed\r\n");  return -1; }
    g_eth_addr = 2;
    delay_us(5000);

    /* 2. Device descriptor at the new address. */
    if (get_descriptor(g_eth_addr, 64, 1, 0, 0, s_descbuf, 18) != 0) {
        uart_puts("USB: device descriptor read FAILED\r\n");  return -1;
    }
    uint32_t vid = (uint32_t)s_descbuf[8]  | ((uint32_t)s_descbuf[9]  << 8);
    uint32_t pid = (uint32_t)s_descbuf[10] | ((uint32_t)s_descbuf[11] << 8);
    uart_puts("USB: device addr 2  idVendor="); put_hex16(vid);
    uart_puts(" idProduct="); put_hex16(pid); uart_puts("\r\n");

    /* 3. Config descriptor: header first (for total length + config value). */
    if (get_descriptor(g_eth_addr, 64, 2, 0, 0, s_descbuf, 9) != 0) {
        uart_puts("USB: config header read FAILED\r\n");  return -1;
    }
    uint8_t  cfgval = s_descbuf[5];
    uint32_t totlen = (uint32_t)s_descbuf[2] | ((uint32_t)s_descbuf[3] << 8);
    if (totlen > sizeof(s_descbuf)) totlen = sizeof(s_descbuf);

    /* 4. Full config descriptor (interface + endpoints). */
    if (get_descriptor(g_eth_addr, 64, 2, 0, 0, s_descbuf, (int)totlen) != 0) {
        uart_puts("USB: full config read FAILED\r\n");  return -1;
    }

    /* 5. Walk the descriptor chain and pick out the endpoints. */
    g_eth_bulk_in = g_eth_bulk_out = g_eth_intr_in = 0;
    for (uint32_t i = 0; i + 2 <= totlen; ) {
        uint8_t dlen  = s_descbuf[i];
        uint8_t dtype = s_descbuf[i + 1];
        if (dlen == 0) break;
        if (dtype == 5 && i + 6 <= totlen) {           /* ENDPOINT descriptor */
            uint8_t  epaddr = s_descbuf[i + 2];
            uint8_t  attr   = s_descbuf[i + 3];
            uint16_t mps    = (uint16_t)(s_descbuf[i + 4] | (s_descbuf[i + 5] << 8));
            uint8_t  ttype  = attr & 0x3u;
            int      is_in  = (epaddr & 0x80u) != 0;
            if (ttype == 2u) {                         /* bulk */
                if (is_in)  { g_eth_bulk_in  = epaddr & 0x0Fu; g_eth_in_mps  = mps; }
                else        { g_eth_bulk_out = epaddr & 0x0Fu; g_eth_out_mps = mps; }
            } else if (ttype == 3u && is_in) {         /* interrupt IN */
                g_eth_intr_in = epaddr & 0x0Fu;
            }
        }
        i += dlen;
    }
    uart_puts("USB: endpoints  bulkIN=ep");  put_dec(g_eth_bulk_in);
    uart_puts(" (");  put_dec(g_eth_in_mps);  uart_puts("B)  bulkOUT=ep"); put_dec(g_eth_bulk_out);
    uart_puts(" (");  put_dec(g_eth_out_mps); uart_puts("B)  intrIN=ep");  put_dec(g_eth_intr_in);
    uart_puts("\r\n");

    /* 6. Configure the device. */
    if (set_configuration(g_eth_addr, cfgval) != 0) {
        uart_puts("USB: device SET_CONFIGURATION failed\r\n");  return -1;
    }

    if (vid == 0x0424u && pid == 0xEC00u)
        uart_puts("USB: ===== SMSC9512 ETHERNET ENUMERATED =====\r\n");
    else
        uart_puts("USB: ===== device enumerated (unexpected VID/PID) =====\r\n");

    
    return (g_eth_bulk_in && g_eth_bulk_out) ? 0 : -1;
}

/* ============= Step 8: SMSC9512 register access (vendor control) ========= */

/* SMSC95xx registers are accessed via vendor control requests on EP0:
 *   WRITE_REGISTER = 0xA0 (OUT), READ_REGISTER = 0xA1 (IN); wIndex = offset. */
#define SMSC_REQ_WRITE_REG   0xA0u
#define SMSC_REQ_READ_REG    0xA1u

/* Register offsets (subset we need now). */
#define SMSC_ID_REV          0x00u
#define SMSC_HW_CFG          0x14u
#define SMSC_PM_CTRL         0x20u
#define SMSC_MAC_CR          0x100u
#define SMSC_ADDRH           0x104u   /* MAC bytes 4-5 */
#define SMSC_ADDRL           0x108u   /* MAC bytes 0-3 */

#define SMSC_HW_CFG_LRST     0x00000008u   /* Lite reset (self-clearing) */
#define SMSC_PM_CTRL_PHY_RST 0x00000010u

/* 4-byte DMA scratch for register payloads (LE; AArch64 is LE, chip is LE). */
static uint32_t __attribute__((aligned(64))) s_reg32;

static int smsc_write_reg(uint32_t index, uint32_t value)
{
    struct usb_setup s = {
        .bmRequestType = 0x40, .bRequest = SMSC_REQ_WRITE_REG,   /* OUT | vendor | device */
        .wValue = 0, .wIndex = (uint16_t)index, .wLength = 4,
    };
    s_reg32 = value;
    return control_xfer(g_eth_addr, 64, &s, &s_reg32, 4);
}
static int smsc_read_reg(uint32_t index, uint32_t *value)
{
    struct usb_setup s = {
        .bmRequestType = 0xC0, .bRequest = SMSC_REQ_READ_REG,    /* IN | vendor | device */
        .wValue = 0, .wIndex = (uint16_t)index, .wLength = 4,
    };
    int r = control_xfer(g_eth_addr, 64, &s, &s_reg32, 4);
    if (r == 0 && value) *value = s_reg32;
    return r;
}

static void put_hex8(uint32_t v)
{
    uint32_t hi = (v >> 4) & 0xF, lo = v & 0xF;
    uart_putc(hi < 10 ? (char)('0' + hi) : (char)('A' + hi - 10));
    uart_putc(lo < 10 ? (char)('0' + lo) : (char)('A' + lo - 10));
}

int usb_eth_chip_probe(void)
{
    if (g_eth_addr == 0) { uart_puts("USB: device not enumerated, skip chip probe\r\n"); return -1; }

    /* 1. Read the chip ID/revision -- proves register READ works. */
    uint32_t id = 0;
    if (smsc_read_reg(SMSC_ID_REV, &id) != 0) {
        uart_puts("SMSC: ID_REV read FAILED\r\n");  return -1;
    }
    uart_puts("SMSC: ID_REV="); put_hex32(id); uart_puts("\r\n");

    /* 2. Lite Reset -- proves register WRITE works (and resets the datapath). */
    if (smsc_write_reg(SMSC_HW_CFG, SMSC_HW_CFG_LRST) != 0) {
        uart_puts("SMSC: HW_CFG write FAILED\r\n");  return -1;
    }
    uint32_t hw = SMSC_HW_CFG_LRST;
    uint64_t t0 = sys_us();
    while (sys_us() - t0 < 100000ULL) {
        if (smsc_read_reg(SMSC_HW_CFG, &hw) != 0) break;
        if (!(hw & SMSC_HW_CFG_LRST)) break;
    }
    if (hw & SMSC_HW_CFG_LRST) uart_puts("SMSC: lite reset TIMEOUT\r\n");
    else                       uart_puts("SMSC: lite reset done\r\n");

    /* 3. Read the MAC address registers (may be firmware-set, or blank after reset). */
    uint32_t lo = 0, hi = 0;
    smsc_read_reg(SMSC_ADDRL, &lo);
    smsc_read_reg(SMSC_ADDRH, &hi);
    uint8_t mac[6] = {
        (uint8_t)(lo), (uint8_t)(lo >> 8), (uint8_t)(lo >> 16), (uint8_t)(lo >> 24),
        (uint8_t)(hi), (uint8_t)(hi >> 8)
    };
    uart_puts("SMSC: MAC=");
    for (int i = 0; i < 6; i++) { put_hex8(mac[i]); if (i < 5) uart_putc(':'); }
    int blank = ((lo == 0 && hi == 0) || (lo == 0xFFFFFFFFu && (hi & 0xFFFFu) == 0xFFFFu));
    uart_puts(blank ? "  (blank - will assign in next step)\r\n" : "\r\n");

    uart_puts("SMSC: ===== REGISTER ACCESS OK =====\r\n");
    
    return 0;
}

/* ============= Step 9: configure the SMSC9512 (MAC, PHY, TX/RX) ========== */

#define SMSC_TX_CFG          0x10u
#define SMSC_BURST_CAP       0x38u
#define SMSC_BULK_IN_DLY     0x6Cu
#define SMSC_AFC_CFG         0x2Cu
#define SMSC_MII_ADDR        0x114u
#define SMSC_MII_DATA        0x118u

#define SMSC_HW_CFG_BIR      0x00001000u   /* bulk-in empty -> send ZLP        */
#define SMSC_HW_CFG_RXDOFF   0x00000600u   /* RX data offset [10:9]            */
#define SMSC_TX_CFG_ON       0x00000004u
#define SMSC_MAC_CR_TXEN     0x00000008u
#define SMSC_MAC_CR_RXEN     0x00000004u
#define SMSC_MAC_CR_FDPX     0x00100000u
#define SMSC_MAC_CR_BCAST    0x00000800u   /* accept broadcast frames (DHCP/ARP) */

#define SMSC_MII_BUSY        0x01u
#define SMSC_MII_WRITE       0x02u
#define SMSC_PHY_ADDR        1u            /* internal PHY is at MII addr 1     */

/* Standard MII PHY registers / bits. */
#define MII_BMCR             0u
#define MII_BMSR             1u
#define MII_PHYID1           2u
#define MII_PHYID2           3u
#define MII_ADVERTISE        4u
#define BMCR_ANRESTART       0x0200u
#define BMCR_ANENABLE        0x1000u
#define BMCR_RESET           0x8000u
#define ADVERTISE_ALL        0x01E1u       /* 10/100, half+full, CSMA           */

static uint8_t g_eth_mac[6];

/* ---- MII (PHY) access through the chip's MII_ADDR/MII_DATA registers ---- */
static int smsc_mii_wait(void)
{
    uint64_t t0 = sys_us();
    for (;;) {
        uint32_t a = SMSC_MII_BUSY;
        if (smsc_read_reg(SMSC_MII_ADDR, &a) != 0) return -1;
        if (!(a & SMSC_MII_BUSY)) return 0;
        if (sys_us() - t0 > 100000ULL) return -1;
    }
}
static int smsc_mdio_read(uint32_t phy, uint32_t idx, uint32_t *val)
{
    if (smsc_mii_wait() < 0) return -1;
    smsc_write_reg(SMSC_MII_ADDR, ((phy & 0x1F) << 11) | ((idx & 0x1F) << 6) | SMSC_MII_BUSY);
    if (smsc_mii_wait() < 0) return -1;
    return smsc_read_reg(SMSC_MII_DATA, val);
}
static int smsc_mdio_write(uint32_t phy, uint32_t idx, uint32_t val)
{
    if (smsc_mii_wait() < 0) return -1;
    smsc_write_reg(SMSC_MII_DATA, val & 0xFFFF);
    smsc_write_reg(SMSC_MII_ADDR,
                   ((phy & 0x1F) << 11) | ((idx & 0x1F) << 6) | SMSC_MII_WRITE | SMSC_MII_BUSY);
    return smsc_mii_wait();
}

/* Mailbox tag 0x00010003 -> board MAC address (the real B8:27:EB:.. Pi MAC). */
static int mbox_get_mac(uint8_t *mac)
{
    int i = 0;
    s_mbox[i++] = 0; s_mbox[i++] = 0;
    s_mbox[i++] = 0x00010003u; s_mbox[i++] = 6; s_mbox[i++] = 0;
    s_mbox[i++] = 0; s_mbox[i++] = 0;            /* 6-byte value buffer (2 words) */
    s_mbox[i++] = 0;                              /* end tag */
    s_mbox[0] = (uint32_t)(i * 4);
    if (!mbox_call(8)) return -1;
    mac[0] = (uint8_t)(s_mbox[5]);       mac[1] = (uint8_t)(s_mbox[5] >> 8);
    mac[2] = (uint8_t)(s_mbox[5] >> 16); mac[3] = (uint8_t)(s_mbox[5] >> 24);
    mac[4] = (uint8_t)(s_mbox[6]);       mac[5] = (uint8_t)(s_mbox[6] >> 8);
    return 0;
}

/* Resolve the MAC once, from the firmware mailbox (no USB needed), with a
 * locally-administered fallback. Called early by genet_get_mac() AND by the
 * chip init, so Mongoose's interface MAC and the chip's filter always match. */
static void usbnet_ensure_mac(void)
{
    if (g_eth_mac[0] | g_eth_mac[1] | g_eth_mac[2] |
        g_eth_mac[3] | g_eth_mac[4] | g_eth_mac[5]) return;       /* already set */
    if (mbox_get_mac(g_eth_mac) != 0 ||
        (g_eth_mac[0] == 0 && g_eth_mac[1] == 0 && g_eth_mac[2] == 0)) {
        g_eth_mac[0] = 0x02; g_eth_mac[1] = 0xCA; g_eth_mac[2] = 0xFE;
        g_eth_mac[3] = 0x00; g_eth_mac[4] = 0x00; g_eth_mac[5] = 0x03;
    }
}

int usb_eth_chip_init(void)
{
    if (g_eth_addr == 0) { uart_puts("USB: device not enumerated, skip chip init\r\n"); return -1; }

    /* 1. MAC address: the same value genet_get_mac() already handed Mongoose. */
    usbnet_ensure_mac();
    uart_puts("SMSC: MAC=");
    for (int i = 0; i < 6; i++) { put_hex8(g_eth_mac[i]); if (i < 5) uart_putc(':'); }
    uart_puts("\r\n");

    /* 2. Program + read back the MAC registers. */
    uint32_t lo = (uint32_t)g_eth_mac[0] | ((uint32_t)g_eth_mac[1] << 8) |
                  ((uint32_t)g_eth_mac[2] << 16) | ((uint32_t)g_eth_mac[3] << 24);
    uint32_t hi = (uint32_t)g_eth_mac[4] | ((uint32_t)g_eth_mac[5] << 8);
    smsc_write_reg(SMSC_ADDRL, lo);
    smsc_write_reg(SMSC_ADDRH, hi);
    uint32_t rl = 0, rh = 0;
    smsc_read_reg(SMSC_ADDRL, &rl);
    smsc_read_reg(SMSC_ADDRH, &rh);
    uart_puts((rl == lo && (rh & 0xFFFFu) == hi) ? "SMSC: MAC programmed OK\r\n"
                                                 : "SMSC: MAC readback MISMATCH\r\n");

    /* 3. Bulk-in behaviour: empty poll returns a ZLP; one frame per transfer. */
    uint32_t hw = 0;
    smsc_read_reg(SMSC_HW_CFG, &hw);
    hw &= ~SMSC_HW_CFG_RXDOFF;                         /* frame right after 4-byte status */
    hw &= ~SMSC_HW_CFG_BIR;                            /* no ZLP-on-empty; empty IN NAKs -> fast halt */
    smsc_write_reg(SMSC_HW_CFG, hw);
    smsc_write_reg(SMSC_BURST_CAP, 0);
    smsc_write_reg(SMSC_BULK_IN_DLY, 0);         /* no coalescing delay */
    smsc_write_reg(SMSC_AFC_CFG, 0x00F830A1u);         /* FIFO flow-control thresholds */

    /* 4. PHY: read its ID (proves MII), reset it, start auto-negotiation. */
    uint32_t id1 = 0, id2 = 0;
    smsc_mdio_read(SMSC_PHY_ADDR, MII_PHYID1, &id1);
    smsc_mdio_read(SMSC_PHY_ADDR, MII_PHYID2, &id2);
    uart_puts("SMSC: PHY ID="); put_hex16(id1); uart_putc(' '); put_hex16(id2); uart_puts("\r\n");

    smsc_mdio_write(SMSC_PHY_ADDR, MII_BMCR, BMCR_RESET);
    uint64_t t0 = sys_us();
    for (;;) {
        uint32_t bmcr = BMCR_RESET;
        smsc_mdio_read(SMSC_PHY_ADDR, MII_BMCR, &bmcr);
        if (!(bmcr & BMCR_RESET)) break;
        if (sys_us() - t0 > 100000ULL) break;
    }
    smsc_mdio_write(SMSC_PHY_ADDR, MII_ADVERTISE, ADVERTISE_ALL);
    smsc_mdio_write(SMSC_PHY_ADDR, MII_BMCR, BMCR_ANENABLE | BMCR_ANRESTART);

    /* 5. Enable transmit and receive (duplex is set when link comes up).
     *    BCAST is required or the chip's perfect filter drops ALL broadcast
     *    frames -- DHCP OFFER and ARP who-has are broadcast, so without this
     *    DHCP never completes and the host is unpingable (RXSTAT frames=0). */
    smsc_write_reg(SMSC_MAC_CR, SMSC_MAC_CR_TXEN | SMSC_MAC_CR_RXEN | SMSC_MAC_CR_BCAST);
    smsc_write_reg(SMSC_TX_CFG, SMSC_TX_CFG_ON);

    uart_puts("SMSC: ===== CHIP CONFIGURED (TX/RX on, autoneg started) =====\r\n");
    
    return 0;
}

/* ============= Step 10: link up + first bulk TX/RX on the wire =========== */

#define SMSC_PHY_SPECIAL     31u           /* SMSC PHY speed/duplex status reg  */
#define SPSCS_SPD_MASK       0x001Cu
#define SPSCS_10HALF         0x0004u
#define SPSCS_10FULL         0x0014u
#define SPSCS_100HALF        0x0008u
#define SPSCS_100FULL        0x0018u
#define BMSR_LSTATUS         0x0004u

/* SMSC bulk framing. */
#define TX_CMD_A_FIRST_SEG   0x00002000u
#define TX_CMD_A_LAST_SEG    0x00001000u
#define RX_STS_FL_MASK       0x3FFF0000u   /* frame length (incl FCS) */
#define RX_STS_ERROR         0x00008000u   /* error summary */

static uint8_t __attribute__((aligned(64))) s_txbuf[8 + 128];
static uint8_t __attribute__((aligned(64))) s_rxbuf[2048];
static uint8_t g_in_toggle, g_out_toggle;

/* One bulk transaction on channel 0; tracks the data toggle via HCTSIZ.PID.
 * For IN, *actual returns the byte count received. */
static int bulk_xfer(int dir_in, uint8_t ep, uint32_t mps, void *buf, int len,
                     uint8_t *toggle, int *actual)
{
    uint32_t pid = (*toggle) ? USB_PID_DATA1 : USB_PID_DATA0;
    int r = chan_xfer(0, g_eth_addr, ep, dir_in, EPTYPE_BULK, mps, pid, buf, len);
    if (r == 0) {
        uint32_t ts = usb_rd(HC_BASE(0) + HCTSIZ);
        uint32_t np = (ts >> HCTSIZ_PID_SHIFT) & 0x3u;
        *toggle = (np == USB_PID_DATA1) ? 1 : 0;
        if (actual) *actual = len - (int)(ts & HCTSIZ_XFERSIZE_MASK);
    }
    return r;
}

static void put_mac6(const uint8_t *m)
{
    for (int i = 0; i < 6; i++) { put_hex8(m[i]); if (i < 5) uart_putc(':'); }
}

int usb_eth_link_test(void)
{
    if (g_eth_addr == 0) { uart_puts("USB: device not ready, skip link test\r\n"); return -1; }

    /* 1. Wait for PHY link (auto-neg takes a couple of seconds). */
    uart_puts("SMSC: waiting for link...\r\n");
    int up = 0; uint32_t bmsr = 0;
    uint64_t t0 = sys_us();
    while (sys_us() - t0 < 5000000ULL) {
        if (smsc_mdio_read(SMSC_PHY_ADDR, MII_BMSR, &bmsr) == 0 && (bmsr & BMSR_LSTATUS)) { up = 1; break; }
        delay_us(100000);
    }
    if (!up) { uart_puts("SMSC: LINK DOWN (cable / switch?)\r\n");  return -1; }

    /* 2. Negotiated speed/duplex -> set MAC duplex. */
    uint32_t sp = 0;
    smsc_mdio_read(SMSC_PHY_ADDR, SMSC_PHY_SPECIAL, &sp);
    uint32_t spd = sp & SPSCS_SPD_MASK;
    const char *desc = (spd == SPSCS_100FULL) ? "100/full" : (spd == SPSCS_100HALF) ? "100/half"
                     : (spd == SPSCS_10FULL)  ? "10/full"  : (spd == SPSCS_10HALF)  ? "10/half" : "?";
    int full = (spd == SPSCS_100FULL || spd == SPSCS_10FULL);
    uint32_t mac_cr = 0;
    smsc_read_reg(SMSC_MAC_CR, &mac_cr);
    if (full) mac_cr |= SMSC_MAC_CR_FDPX; else mac_cr &= ~SMSC_MAC_CR_FDPX;
    smsc_write_reg(SMSC_MAC_CR, mac_cr);
    uart_puts("SMSC: LINK UP "); uart_puts(desc); uart_puts("\r\n");

    /* 3. Send one broadcast ARP request -> proves the TX path. */
    uint8_t *f = s_txbuf + 8;                         /* frame starts after TX cmd */
    for (int i = 0; i < 6; i++) f[i] = 0xFF;          /* dst broadcast */
    for (int i = 0; i < 6; i++) f[6 + i] = g_eth_mac[i];
    f[12] = 0x08; f[13] = 0x06;                       /* ethertype ARP */
    f[14] = 0x00; f[15] = 0x01; f[16] = 0x08; f[17] = 0x00;  /* HW=Eth, proto=IP */
    f[18] = 6; f[19] = 4; f[20] = 0x00; f[21] = 0x01;        /* hlen,plen,op=req */
    for (int i = 0; i < 6; i++) f[22 + i] = g_eth_mac[i];    /* sender HW */
    f[28]=192; f[29]=168; f[30]=123; f[31]=99;               /* sender IP 192.168.123.99 */
    for (int i = 0; i < 6; i++) f[32 + i] = 0;               /* target HW (unknown) */
    f[38]=192; f[39]=168; f[40]=123; f[41]=254;              /* target IP = gateway */
    int flen = 42;
    uint32_t cmda = (uint32_t)flen | TX_CMD_A_FIRST_SEG | TX_CMD_A_LAST_SEG;
    uint32_t cmdb = (uint32_t)flen;
    s_txbuf[0]=(uint8_t)cmda; s_txbuf[1]=(uint8_t)(cmda>>8); s_txbuf[2]=(uint8_t)(cmda>>16); s_txbuf[3]=(uint8_t)(cmda>>24);
    s_txbuf[4]=(uint8_t)cmdb; s_txbuf[5]=(uint8_t)(cmdb>>8); s_txbuf[6]=(uint8_t)(cmdb>>16); s_txbuf[7]=(uint8_t)(cmdb>>24);

    if (bulk_xfer(0, g_eth_bulk_out, g_eth_out_mps, s_txbuf, 8 + flen, &g_out_toggle, 0) == 0)
        uart_puts("SMSC: broadcast ARP sent (TX ok)\r\n");
    else
        uart_puts("SMSC: TX FAILED\r\n");

    /* 4. Poll the bulk-IN for real frames off the network (~3s). */
    int got = 0;
    t0 = sys_us();
    while (sys_us() - t0 < 3000000ULL && got < 5) {
        int actual = 0;
        if (bulk_xfer(1, g_eth_bulk_in, g_eth_in_mps, s_rxbuf, (int)sizeof(s_rxbuf),
                      &g_in_toggle, &actual) != 0) { delay_us(2000); continue; }
        if (actual <= 4) { delay_us(2000); continue; }    /* ZLP / no frame */

        uint32_t rxs = (uint32_t)s_rxbuf[0] | ((uint32_t)s_rxbuf[1] << 8) |
                       ((uint32_t)s_rxbuf[2] << 16) | ((uint32_t)s_rxbuf[3] << 24);
        int fl = (int)((rxs & RX_STS_FL_MASK) >> 16);
        uint8_t *fr = s_rxbuf + 4;
        uart_puts("SMSC: RX len="); put_dec((uint32_t)fl);
        uart_puts(" dst="); put_mac6(fr);
        uart_puts(" src="); put_mac6(fr + 6);
        uart_puts(" type="); put_hex8(fr[12]); put_hex8(fr[13]);
        if (rxs & RX_STS_ERROR) uart_puts(" ERR");
        uart_puts("\r\n");
        got++;
    }

    if (got) uart_puts("SMSC: ===== WIRE ALIVE: RX/TX working =====\r\n");
    else     uart_puts("SMSC: TX ok but no RX (very quiet network?)\r\n");
    
    return 0;
}

/* ============= Step 11: raw-frame API for the Mongoose stack ============= */

static uint8_t __attribute__((aligned(64))) s_txframe[8 + 1600];

static void u_memcpy(void *d, const void *sb, unsigned long n)
{
    uint8_t *dp = (uint8_t *)d; const uint8_t *sp = (const uint8_t *)sb;
    while (n--) *dp++ = *sp++;
}

/* Wait (<=5s) for PHY link, set MAC duplex to match. 1 if up, else 0. */
static int smsc_link_setup(void)
{
    int up = 0; uint32_t bmsr = 0;
    uint64_t t0 = sys_us();
    while (sys_us() - t0 < 5000000ULL) {
        if (smsc_mdio_read(SMSC_PHY_ADDR, MII_BMSR, &bmsr) == 0 && (bmsr & BMSR_LSTATUS)) { up = 1; break; }
        delay_us(100000);
    }
    if (!up) { uart_puts("USBNET: link still down (will retry in up())\r\n"); return 0; }
    uint32_t sp = 0;
    smsc_mdio_read(SMSC_PHY_ADDR, SMSC_PHY_SPECIAL, &sp);
    uint32_t spd = sp & SPSCS_SPD_MASK;
    int full = (spd == SPSCS_100FULL || spd == SPSCS_10FULL);
    uint32_t mac_cr = 0;
    smsc_read_reg(SMSC_MAC_CR, &mac_cr);
    if (full) mac_cr |= SMSC_MAC_CR_FDPX; else mac_cr &= ~SMSC_MAC_CR_FDPX;
    smsc_write_reg(SMSC_MAC_CR, mac_cr);
    const char *desc = (spd==SPSCS_100FULL)?"100/full":(spd==SPSCS_100HALF)?"100/half"
                     : (spd==SPSCS_10FULL)?"10/full":(spd==SPSCS_10HALF)?"10/half":"?";
    uart_puts("USBNET: LINK UP "); uart_puts(desc); uart_puts("\r\n");
    return 1;
}

/* Full bring-up: DWC2 host -> hub -> SMSC9512 -> link. Idempotent.
 * Returns 100 (Mbps) on success, -1 on failure. */
int usbnet_init(void)
{
    static int s_ready = 0;
    if (s_ready) return 100;

    if (usb_dwc2_probe()     != 0) return -1;
    if (usb_host_init()      != 0) return -1;
    if (usb_port_init()      != 0) return -1;
    if (usb_hub_init()       != 0) return -1;
    if (usb_hub_scan()       != 0) return -1;
    if (usb_eth_enum()       != 0) return -1;
    if (usb_eth_chip_probe() != 0) return -1;
    if (usb_eth_chip_init()  != 0) return -1;
    smsc_link_setup();

    s_ready = 1;
    uart_puts("USBNET: SMSC9512 ready, link up\r\n");
    return 100;
}

/* Send one Ethernet frame (no FCS): prepend the 8-byte SMSC TX command. */
unsigned long usbnet_tx(const void *frame, unsigned long len)
{
    if (g_eth_addr == 0 || len == 0 || len > 1514) return 0;
    uint32_t a = (uint32_t)len | TX_CMD_A_FIRST_SEG | TX_CMD_A_LAST_SEG;
    uint32_t b = (uint32_t)len;
    s_txframe[0]=(uint8_t)a; s_txframe[1]=(uint8_t)(a>>8); s_txframe[2]=(uint8_t)(a>>16); s_txframe[3]=(uint8_t)(a>>24);
    s_txframe[4]=(uint8_t)b; s_txframe[5]=(uint8_t)(b>>8); s_txframe[6]=(uint8_t)(b>>16); s_txframe[7]=(uint8_t)(b>>24);
    u_memcpy(s_txframe + 8, frame, len);
    if (bulk_xfer(0, g_eth_bulk_out, g_eth_out_mps, s_txframe, 8 + (int)len, &g_out_toggle, 0) == 0)
        return len;
    return 0;
}

/* Poll one received frame (no FCS) into buf; 0 if none available. */
/* One bulk-IN attempt for RX polling. Returns bytes received (0 = empty via
 * NAK or ZLP), or -1 on error. Toggle advances only on a completed packet. */
static int bulk_in_once(uint8_t ep, uint32_t mps, void *buf, int len, uint8_t *toggle)
{
    uint32_t base = HC_BASE(0);
    chan_disable(0);

    uint32_t pid = (*toggle) ? USB_PID_DATA1 : USB_PID_DATA0;
    int pktcnt = (len + (int)mps - 1) / (int)mps;
    if (pktcnt < 1) pktcnt = 1;

    usb_wr(base + HCINT, 0xFFFFFFFFu);
    usb_wr(base + HCINTMSK, 0);
    usb_wr(base + HCTSIZ,
           ((pid & 0x3u) << HCTSIZ_PID_SHIFT) |
           (((uint32_t)pktcnt & 0x3FFu) << HCTSIZ_PKTCNT_SHIFT) |
           ((uint32_t)len & HCTSIZ_XFERSIZE_MASK));
    usb_wr(base + HCDMA, USB_BUS_ADDR(buf));
    usb_wr(base + HCSPLT, 0);
    usb_wr(base + HCCHAR,
           (mps & HCCHAR_MPS_MASK)
         | (((uint32_t)ep & 0xF) << HCCHAR_EPNUM_SHIFT)
         | HCCHAR_EPDIR_IN
         | (EPTYPE_BULK << HCCHAR_EPTYPE_SHIFT)
         | (1u << HCCHAR_MC_SHIFT)
         | (((uint32_t)g_eth_addr & 0x7Fu) << HCCHAR_DEVADDR_SHIFT)
         | HCCHAR_CHENA);

    uint64_t t0 = sys_us();
    uint32_t hcint;
    for (;;) {
        hcint = usb_rd(base + HCINT);
        if (hcint & HCINT_CHHLTD) break;
        if (sys_us() - t0 > 800ULL) { chan_disable(0); return 0; }   /* safety bail; NAK normally halts first */
    }
    usb_wr(base + HCINT, 0xFFFFFFFFu);

    if (hcint & HCINT_XFERCOMPL) {
        uint32_t ts = usb_rd(base + HCTSIZ);
        uint32_t np = (ts >> HCTSIZ_PID_SHIFT) & 0x3u;
        *toggle = (np == USB_PID_DATA1) ? 1 : 0;       /* advance toggle */
        return len - (int)(ts & HCTSIZ_XFERSIZE_MASK); /* 0 if ZLP */
    }
    if (hcint & HCINT_NAK) return 0;                   /* no data; toggle unchanged */
    chan_disable(0);
    return -1;                                         /* STALL / xact error */
}

unsigned long usbnet_rx(void *buf, unsigned long max)
{
    if (g_eth_addr == 0) return 0;

    uint64_t _t = sys_us();
    int actual = bulk_in_once(g_eth_bulk_in, g_eth_in_mps, s_rxbuf, (int)sizeof(s_rxbuf), &g_in_toggle);
    uint32_t _dt = (uint32_t)(sys_us() - _t);

    /* --- RX poll stats (once/sec): calls, frames, avg/max poll us, near-bail count --- */
    static uint32_t s_calls, s_frames, s_sum, s_max, s_bail;
    static uint64_t s_t0;
    s_calls++; s_sum += _dt; if (_dt > s_max) s_max = _dt;
    if (actual > 4) s_frames++;
    if (_dt >= 700u) s_bail++;
    if (s_t0 == 0) s_t0 = _t;
    if (_t - s_t0 >= 1000000ULL) {
        uart_puts("RXSTAT calls="); put_dec(s_calls);
        uart_puts(" frames=");      put_dec(s_frames);
        uart_puts(" avgus=");       put_dec(s_calls ? s_sum / s_calls : 0);
        uart_puts(" maxus=");       put_dec(s_max);
        uart_puts(" bails=");       put_dec(s_bail);
        uart_puts("\r\n");
        s_calls = s_frames = s_sum = s_max = s_bail = 0; s_t0 = _t;
    }

    if (actual <= 4) return 0;                          /* error, empty, or ZLP */

    uint32_t rxs = (uint32_t)s_rxbuf[0] | ((uint32_t)s_rxbuf[1]<<8) |
                   ((uint32_t)s_rxbuf[2]<<16) | ((uint32_t)s_rxbuf[3]<<24);
    int fl = (int)((rxs & RX_STS_FL_MASK) >> 16);       /* length incl 4-byte FCS */
    int framelen = fl - 4;

    /* --- per-frame diag, capped: what is actually arriving? --- */
    static int s_fd = 0;
    if (s_fd < 40) {
        s_fd++;
        uint16_t et = (framelen >= 14) ?
            (uint16_t)(((uint16_t)s_rxbuf[4+12] << 8) | s_rxbuf[4+13]) : 0;
        uart_puts("RXF len="); put_dec((uint32_t)framelen);
        uart_puts(" actual="); put_dec((uint32_t)actual);
        uart_puts(" type=");   put_hex16(et);
        uart_puts(" dst=");    put_hex16((uint16_t)(((uint16_t)s_rxbuf[4]<<8)|s_rxbuf[5]));
        uart_puts(" rxs=");    put_hex32(rxs);
        uart_puts(" max=");    put_dec((uint32_t)max);
        uart_puts((rxs & RX_STS_ERROR) ? " ERR\r\n" : (((unsigned long)framelen > max) ? " TOOBIG\r\n" : " ok\r\n"));
    }

    /* --- deep dump for OFFER-sized IP frames: are the body bytes coherent? --- */
    static int s_hd = 0;
    {
        uint16_t et = (framelen >= 14) ?
            (uint16_t)(((uint16_t)s_rxbuf[4+12] << 8) | s_rxbuf[4+13]) : 0;
        if (s_hd < 4 && et == 0x0800 && framelen > 300) {
            s_hd++;
            int n = (actual < 60) ? actual : 60;   /* eth14+ip20+udp8+dhcp14=56 */
            uart_puts("HEXDUMP actual="); put_dec((uint32_t)actual);
            uart_puts(" framelen="); put_dec((uint32_t)framelen); uart_puts("\r\n");
            for (int i = 0; i < n; i++) {
                put_hex8(s_rxbuf[i]);
                uart_puts((i % 16 == 15) ? "\r\n" : " ");
            }
            uart_puts("\r\n");
        }
    }

    if (rxs & RX_STS_ERROR) return 0;
    if (framelen <= 0 || (unsigned long)framelen > max) return 0;
    u_memcpy(buf, s_rxbuf + 4, (unsigned long)framelen);
    return (unsigned long)framelen;
}

/* Link state, cached and refreshed at most once a second (MII is slow). */
int usbnet_up(void)
{
    static uint64_t s_last;
    static int s_cached;
    uint64_t now = sys_us();
    if (s_last == 0 || now - s_last > 1000000ULL) {
        uint32_t bmsr = 0;
        if (smsc_mdio_read(SMSC_PHY_ADDR, MII_BMSR, &bmsr) == 0)
            s_cached = (bmsr & BMSR_LSTATUS) ? 1 : 0;
        s_last = now;
    }
    return s_cached;
}

void usbnet_get_mac(unsigned char mac[6])
{
    usbnet_ensure_mac();
    for (int i = 0; i < 6; i++) mac[i] = g_eth_mac[i];
}

#endif