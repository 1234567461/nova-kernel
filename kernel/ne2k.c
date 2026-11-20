/* NovaOS - NE2000 / DP8390 ethernet driver (polled, 16-bit data port)
 *
 * The NE2000 is the one NIC that both QEMU (model=ne2k_pci / ne2k_isa) and
 * v86 expose, so a single driver gives the kernel a real network on every
 * target we run on.
 *
 * Packet memory on the card is a 16KB ring addressed in 256-byte "pages":
 *   0x40        transmit buffer (page 0x40)
 *   0x46..0x80  receive ring
 *
 * Everything is polled: IMR stays 0 and net_poll() drains the ring, which
 * keeps the driver free of interrupt-plumbing surprises and works the same
 * way whether or not the IRQ line is wired.
 */
#include "ne2k.h"
#include "pci.h"
#include "io.h"
#include "printf.h"

#define NE_CR      0x00
#define NE_PSTART  0x01
#define NE_PSTOP   0x02
#define NE_BNRY    0x03
#define NE_TPSR    0x04
#define NE_TBCR0   0x05
#define NE_TBCR1   0x06
#define NE_ISR     0x07
#define NE_RSAR0   0x08
#define NE_RSAR1   0x09
#define NE_RBCR0   0x0A
#define NE_RBCR1   0x0B
#define NE_RCR     0x0C        /* receive configuration (page 0) */
#define NE_TCR     0x0D
#define NE_DCR     0x0E
#define NE_IMR     0x0F
#define NE_DATA    0x10

/* page 1 */
#define NE_PAR0    0x01
#define NE_CURR    0x07
#define NE_MAR0    0x08

#define NE_CR_STP  0x01
#define NE_CR_STA  0x02
#define NE_CR_TXP  0x04
#define NE_CR_RD0  0x08        /* RS = 001: remote read  (NIC -> host) */
#define NE_CR_RD1  0x10        /* RS = 010: remote write (host -> NIC) */
#define NE_CR_PAGE1 0x40

#define TX_PAGE   0x40
#define RX_START  0x46
#define RX_STOP   0x80

static u16 base;
static u8  mac[6];
static u8  bnry = RX_START;
static int present;
static int irq_line;
static u32 tx_count, rx_count;

static void delay(volatile u32 n) { while (n--) io_wait(); }

/* --- remote DMA ------------------------------------------------------- */
static void dma_setup(u16 addr, u16 len, u8 cr_bits) {
    outb(base + NE_CR, NE_CR_STA);            /* page 0, no DMA */
    outb(base + NE_RBCR0, (u8)(len & 0xFF));
    outb(base + NE_RBCR1, (u8)(len >> 8));
    outb(base + NE_RSAR0, (u8)(addr & 0xFF));
    outb(base + NE_RSAR1, (u8)(addr >> 8));
    outb(base + NE_CR, NE_CR_STA | cr_bits);
}

static void dma_wait(void) {
    for (int i = 0; i < 20000; i++)
        if (inb(base + NE_ISR) & 0x40) break;   /* RDC */
    outb(base + NE_ISR, 0x40);
}

static void dma_read(u16 addr, u8 *buf, u16 len) {
    dma_setup(addr, len, NE_CR_RD0);
    for (u16 i = 0; i < len; i += 2) {
        u16 w = inw(base + NE_DATA);
        buf[i] = (u8)(w & 0xFF);
        if (i + 1 < len) buf[i + 1] = (u8)(w >> 8);
    }
    dma_wait();
}

static void dma_write(u16 addr, const u8 *buf, u16 len) {
    dma_setup(addr, len, NE_CR_RD1);
    for (u16 i = 0; i < len; i += 2) {
        u16 w = buf[i];
        if (i + 1 < len) w |= (u16)buf[i + 1] << 8;
        outw(base + NE_DATA, w);
    }
    dma_wait();
}

/* --- probe / init ----------------------------------------------------- */
static void read_mac(void) {
    outb(base + NE_CR, 0x21 | NE_CR_PAGE1);      /* stop, no dma, page 1 */
    for (int i = 0; i < 6; i++) mac[i] = inb(base + NE_PAR0 + i);
    kprintf("[ne2k] page1 CR=%02x CURR=%02x MAR0=%02x\n",
            inb(base + NE_CR), inb(base + NE_CURR), inb(base + NE_MAR0));
    outb(base + NE_CR, 0x21);
}

static void hw_reset(void) {
    u8 v = inb(base + 0x1F);       /* read the reset port */
    outb(base + 0x1F, v);
    delay(2000);
    outb(base + NE_ISR, 0xFF);     /* clear any pending interrupt */
    delay(2000);
}

static int init_at(u16 io, int irq) {
    base = io;
    irq_line = irq;

    hw_reset();
    kprintf("[ne2k] post-reset CR=%02x ISR=%02x\n", inb(base + NE_CR), inb(base + NE_ISR));

    outb(base + NE_CR, 0x21);               /* stop, no dma, page 0 */
    outb(base + NE_DCR, 0x49);              /* 16-bit DMA, normal */
    outb(base + NE_RBCR0, 0x00);
    outb(base + NE_RBCR1, 0x00);
    outb(base + NE_RCR, 0x20);              /* monitor mode while probing */
    outb(base + NE_TCR, 0x02);              /* internal loopback */
    outb(base + NE_TPSR, TX_PAGE);
    outb(base + NE_PSTART, RX_START);
    outb(base + NE_PSTOP, RX_STOP);
    outb(base + NE_BNRY, RX_START);
    outb(base + NE_ISR, 0xFF);
    outb(base + NE_IMR, 0x00);              /* polled */

    read_mac();

    /* an all-zero or all-0xFF MAC means nothing is really there */
    int zero = 1, ones = 1;
    for (int i = 0; i < 6; i++) {
        if (mac[i] != 0x00) zero = 0;
        if (mac[i] != 0xFF) ones = 0;
    }
    if (zero || ones) return 0;

    outb(base + NE_CR, NE_CR_STP | NE_CR_PAGE1);
    outb(base + NE_CURR, RX_START + 1);
    for (int i = 0; i < 8; i++) outb(base + NE_MAR0 + i, 0x00);
    outb(base + NE_CR, NE_CR_STP);

    bnry = RX_START;
    outb(base + NE_BNRY, bnry);
    outb(base + NE_ISR, 0xFF);
    outb(base + NE_RCR, 0x04);              /* accept broadcast */
    outb(base + NE_TCR, 0x00);              /* normal transmit */
    outb(base + NE_CR, NE_CR_STA);          /* go */
    delay(1000);
    outb(base + NE_ISR, 0xFF);

    present = 1;
    return 1;
}

int ne2k_init(void) {
    pci_dev_t d;

    present = 0;
    /* PCI variants: Realtek 8029 (NE2000 clone) */
    if (pci_find(0x10EC, 0x8029, &d)) {
        pci_enable(&d);
        u32 io = pci_bar_io(&d, 0);
        kprintf("[ne2k] pci %02x:%02x.%x bar0=%x io=%x irq=%u\n",
                d.bus, d.dev, d.fn, d.bar[0], io, d.irq);
        if (io && init_at((u16)io, d.irq)) return 1;
        kprintf("[ne2k] pci init failed, mac %02x:%02x:%02x:%02x:%02x:%02x\n",
                mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    } else {
        kprintf("[ne2k] no pci 8029 found\n");
    }
    /* ISA / v86 default jumper position */
    if (init_at(0x300, 9)) return 1;
    if (init_at(0x280, 9)) return 1;
    present = 0;
    return 0;
}

int ne2k_present(void)     { return present; }
void ne2k_mac(u8 out[6])   { for (int i = 0; i < 6; i++) out[i] = mac[i]; }
u16 ne2k_io_base(void)     { return base; }
u32 ne2k_tx_count(void)    { return tx_count; }
u32 ne2k_rx_count(void)    { return rx_count; }
int ne2k_irq_line(void)    { return irq_line; }

/* --- transmit --------------------------------------------------------- */
int ne2k_send(const u8 *frame, u16 len) {
    if (!present || len < 14 || len > 1514) return 0;

    dma_write((u16)TX_PAGE << 8, frame, len);

    outb(base + NE_TPSR, TX_PAGE);
    outb(base + NE_TBCR0, (u8)(len & 0xFF));
    outb(base + NE_TBCR1, (u8)(len >> 8));
    outb(base + NE_CR, NE_CR_STA | NE_CR_TXP);

    for (int i = 0; i < 40000; i++) {
        u8 isr = inb(base + NE_ISR);
        if (isr & 0x02) {                    /* PTX: transmitted */
            outb(base + NE_ISR, 0x02);
            tx_count++;
            return 1;
        }
        if (isr & 0x04) {                    /* TXE: error */
            outb(base + NE_ISR, 0x04);
            return 0;
        }
    }
    return 0;
}

/* --- receive ---------------------------------------------------------- */
int ne2k_recv(u8 *buf, u16 max) {
    if (!present) return 0;

    outb(base + NE_CR, NE_CR_STA | NE_CR_PAGE1);
    u8 curr = inb(base + NE_CURR);
    outb(base + NE_CR, NE_CR_STA);

    if (curr == (u8)(bnry + 1)) return 0;    /* ring empty */

    u8 hdr[4];
    dma_read((u16)bnry << 8, hdr, 4);

    u16 len = (u16)hdr[2] | ((u16)hdr[3] << 8);
    u8  next = hdr[1];

    if (len < 4 || len > 1518 || next < RX_START || next > RX_STOP) {
        /* corrupt ring pointer: resynchronise on the card's own cursor */
        bnry = (u8)(curr - 1);
        if (bnry < RX_START) bnry = RX_STOP - 1;
        outb(base + NE_BNRY, bnry);
        return 0;
    }

    len -= 4;                                /* strip the 4-byte header */
    if (len > max) len = max;

    u16 start = (u16)(((u16)bnry << 8) + 4);
    u16 ring_end = (u16)RX_STOP << 8;
    if (start + len > ring_end) {
        u16 first = (u16)(ring_end - start);
        dma_read(start, buf, first);
        dma_read((u16)RX_START << 8, buf + first, (u16)(len - first));
    } else {
        dma_read(start, buf, len);
    }

    bnry = (u8)(next - 1);
    if (bnry < RX_START) bnry = RX_STOP - 1;
    outb(base + NE_BNRY, bnry);
    rx_count++;
    return (int)len;
}
