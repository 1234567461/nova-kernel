/* NovaOS - RTL8139 driver
 *
 * The 8139 is the most forgiving NIC to drive from a hobby kernel: after a
 * soft reset the MAC is readable straight from IDR0-5 (no EEPROM dance),
 * and both directions are plain memory copies through a ring the card
 * owns.  Everything is polled - IMR stays 0, rtl_recv() drains the ring.
 */
#include "rtl8139.h"
#include "pci.h"
#include "io.h"
#include "string.h"
#include "printf.h"

#define R_IDR0      0x00
#define R_TSD0      0x10
#define R_TSAD0     0x20
#define R_RBSTART   0x30
#define R_CR        0x37
#define R_CAPR      0x38
#define R_CBR       0x3A
#define R_IMR       0x3C
#define R_ISR       0x3E
#define R_TCR       0x40
#define R_RCR       0x44
#define R_CFG9346   0x50
#define R_CONFIG1   0x52

#define RX_RING    (8192 + 16)   /* RBLEN=0 means 8K+16 bytes */
#define RX_BUF_SIZE RX_RING
#define TX_SLOTS    4
#define TX_SLOT_SZ  2048

static u16 io;
static u8  mac[6];
static int present;
static int irq_line;
static u32 tx_cur, tx_count, rx_count;
static u16 rx_off;

static u8 rx_ring[RX_BUF_SIZE] __attribute__((aligned(8192)));
static u8 tx_buf[TX_SLOTS][TX_SLOT_SZ] __attribute__((aligned(16)));

static void delay(volatile u32 n) { while (n--) io_wait(); }

static int init_at(u16 port, int irq) {
    io = port;
    irq_line = irq;

    /* Unlock the configuration registers.  QEMU models the write-enable
     * latch as "clock_enabled": while it is clear, rtl8139_do_receive()
     * returns -1 and every inbound frame is thrown away (can_receive()
     * even reports "ready", so the loss is silent).  Real silicon needs
     * the same unlock before Config1/RCR/RBSTART are writable. */
    outb(io + R_CFG9346, 0xC0);

    /* wake up: clear CONFIG1 sleep/LWAK bits */
    outb(io + R_CONFIG1, 0x00);

    /* soft reset */
    outb(io + R_CR, 0x10);
    for (int i = 0; i < 100000; i++) {
        if (!(inb(io + R_CR) & 0x10)) break;
        io_wait();
    }
    if (inb(io + R_CR) & 0x10) {
        kprintf("[rtl8139] reset never completed (CR=%02x)\n", inb(io + R_CR));
        return 0;
    }

    for (int i = 0; i < 6; i++) mac[i] = inb(io + R_IDR0 + i);
    kprintf("[rtl8139] io=%x irq=%u mac=%02x:%02x:%02x:%02x:%02x:%02x\n",
            io, irq, mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);

    int zero = 1, ones = 1;
    for (int i = 0; i < 6; i++) {
        if (mac[i] != 0x00) zero = 0;
        if (mac[i] != 0xFF) ones = 0;
    }
    if (zero || ones) return 0;

    outb(io + R_IMR, 0x00);              /* polled */
    outw(io + R_ISR, 0xFFFF);
    outl(io + R_TCR, 0x03000700);        /* IFG 11, 8 retries, 1.5KB DMA burst */
    /* RCR must assert AB|AM|APM|AAP (bits 24-27) or the card drops every
     * frame - a receiver that never matches is indistinguishable from a
     * dead link.  Bit 7 turns on ring wrap; RBLEN bits 0-2 stay 0 = 8K+16,
     * which is exactly the size of rx_ring[]. */
    /* Receive configuration.  Bits 8-10 select the ring size (0 = 8K+16,
     * matching rx_ring[]); everything else that matters for a polled
     * driver is on: AB/AM/APM/AAP (24-27), the RX FIFO early thresholds
     * (12-17), WRAP (7) and the low control bits (0-5).  Leaving the FIFO
     * threshold at zero makes the chip wait for a full FIFO before DMAing
     * a frame, so small packets (ARP, ICMP, UDP ACKs) never arrive while
     * big ones do - a maddening half-working link. */
    outl(io + R_RCR, 0x0F03F8BF);
    outl(io + R_RBSTART, (u32)rx_ring);
    /* CAPR is the read pointer minus 16 bytes.  Programming 0 here tells
     * the card the buffer is completely full (the free-space calculation
     * is (write_ptr - read_ptr) mod ring), so can_receive() returns 0 and
     * not a single frame is ever DMAed into the ring.  0xFFF0 == -16
     * leaves the 16-byte headroom the chip wants. */
    outw(io + R_CAPR, 0xFFF0);
    outb(io + R_CR, 0x0C);               /* RX + TX enable */
    /* Deliberately leave Cfg9346 unlocked (0xC0).  Locking it clears the
     * chip's write-enable latch, which QEMU also treats as "stop the
     * receiver clock", so every inbound frame is dropped even though CR
     * still says RX enabled.  Leaving it unlocked keeps both behaviours
     * (real silicon: just writable config regs). */

    rx_off = 0;
    tx_cur = 0;
    present = 1;
    return 1;
}

int rtl_init(void) {
    pci_dev_t d;
    present = 0;

    if (!pci_find(0x10EC, 0x8139, &d)) {
        kprintf("[rtl8139] no PCI device\n");
        return 0;
    }
    pci_enable(&d);
    u32 bar = pci_bar_io(&d, 0);
    if (!bar) return 0;
    if (!init_at((u16)bar, d.irq)) {
        kprintf("[rtl8139] init failed (mac %02x:%02x:%02x:%02x:%02x:%02x)\n",
                mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
        present = 0;
        return 0;
    }
    return 1;
}

void rtl_dbg(void) {
    if (!present) { kprintf("[rtl] not present\n"); return; }
    kprintf("[rtl] CR=%02x CBR=%04x CAPR=%04x ISR=%04x RCR=%08x ring=%08x rx_off=%u rx=%u tx=%u\n",
            inb(io + R_CR), inw(io + R_CBR), inw(io + R_CAPR), inw(io + R_ISR),
            inl(io + R_RCR), (u32)rx_ring, (u32)rx_off, rx_count, tx_count);
}

int rtl_present(void)  { return present; }
void rtl_mac(u8 out[6]){ for (int i = 0; i < 6; i++) out[i] = mac[i]; }
u16 rtl_io_base(void)  { return io; }
int rtl_irq_line(void) { return irq_line; }
u32 rtl_tx_count(void) { return tx_count; }
u32 rtl_rx_count(void) { return rx_count; }

int rtl_send(const u8 *frame, u16 len) {
    if (!present || len < 14 || len > 1514) return 0;

    u32 slot = tx_cur;
    memcpy(tx_buf[slot], frame, len);
    if (len < 60) {                        /* pad small frames */
        memset(tx_buf[slot] + len, 0, 60 - len);
        len = 60;
    }

    outl(io + R_TSAD0 + slot * 4, (u32)tx_buf[slot]);
    outl(io + R_TSD0  + slot * 4, (u32)len);

    for (int i = 0; i < 100000; i++) {
        if (inl(io + R_TSD0 + slot * 4) & 0x8000) {   /* OWN: done */
            tx_count++;
            tx_cur = (slot + 1) % TX_SLOTS;
            return 1;
        }
        delay(10);
    }
    return 0;
}

int rtl_recv(u8 *buf, u16 max) {
    if (!present) return 0;

    u16 cbr = inw(io + R_CBR);
    if (cbr == rx_off) return 0;
    if (cbr >= RX_RING) { rx_off = 0; outw(io + R_CAPR, 0xFFF0); return 0; }

    u8 *p = rx_ring + rx_off;
    u16 status = (u16)(p[0] | (p[1] << 8));
    u16 len    = (u16)(p[2] | (p[3] << 8));

    if (!(status & 0x01) || len < 4 || len > 1600) {
        /* resynchronise: drop the ring and start over */
        rx_off = 0;
        outw(io + R_CAPR, 0xFFF0);
        return 0;
    }

    u16 plen = (u16)(len - 4);
    if (plen > max) plen = max;

    /* the 4-byte header plus payload can straddle the ring wrap */
    u16 start = (u16)(rx_off + 4);
    if (start + plen > RX_RING) {
        u16 first = (u16)(RX_RING - start);
        memcpy(buf, rx_ring + start, first);
        memcpy(buf + first, rx_ring, (u32)(plen - first));
    } else {
        memcpy(buf, rx_ring + start, plen);
    }

    rx_off = (u16)((rx_off + len + 4 + 3) & ~3u);
    rx_off %= RX_RING;
    outw(io + R_CAPR, (u16)(rx_off - 16));

    rx_count++;
    return (int)(len - 4);
}
