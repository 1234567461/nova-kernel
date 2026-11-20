/* NovaOS - PCI bus enumeration
 *
 * Only what a driver needs to bind: walk bus 0..255, read the header, and
 * hand back vendor/device/BARs/IRQ.  Configuration space is reached through
 * the legacy CAM ports 0xCF8 (address) / 0xCFC (data), which every chipset
 * since the 430FX implements.
 */
#include "pci.h"
#include "io.h"

#define PCI_ADDR 0xCF8
#define PCI_DATA 0xCFC

u32 pci_read32(u8 bus, u8 dev, u8 fn, u8 off) {
    u32 addr = 0x80000000u
             | ((u32)bus << 16)
             | ((u32)(dev & 0x1F) << 11)
             | ((u32)(fn & 0x07) << 8)
             | (off & 0xFC);
    outl(PCI_ADDR, addr);
    return inl(PCI_DATA);
}

u16 pci_read16(u8 bus, u8 dev, u8 fn, u8 off) {
    u32 v = pci_read32(bus, dev, fn, off & 0xFC);
    return (u16)((v >> ((off & 2) * 8)) & 0xFFFF);
}

void pci_write32(u8 bus, u8 dev, u8 fn, u8 off, u32 val) {
    u32 addr = 0x80000000u
             | ((u32)bus << 16)
             | ((u32)(dev & 0x1F) << 11)
             | ((u32)(fn & 0x07) << 8)
             | (off & 0xFC);
    outl(PCI_ADDR, addr);
    outl(PCI_DATA, val);
}

void pci_init(void) {
    /* nothing global to set up: enumeration is stateless */
}

static int read_dev(u8 bus, u8 dev, u8 fn, pci_dev_t *d) {
    u16 vendor = pci_read16(bus, dev, fn, 0x00);
    if (vendor == 0xFFFF) return 0;

    d->bus = bus; d->dev = dev; d->fn = fn;
    d->vendor  = vendor;
    d->device  = pci_read16(bus, dev, fn, 0x02);
    u32 cls    = pci_read32(bus, dev, fn, 0x08);
    d->class   = (u8)(cls >> 24);
    d->subclass= (u8)(cls >> 16);
    d->prog_if = (u8)(cls >> 8);
    d->irq     = (u8)(pci_read32(bus, dev, fn, 0x3C) & 0xFF);

    for (int i = 0; i < 6; i++)
        d->bar[i] = pci_read32(bus, dev, fn, 0x10 + i * 4);
    return 1;
}

int pci_scan(void (*cb)(const pci_dev_t *d)) {
    int n = 0;
    for (int bus = 0; bus < 8; bus++) {          /* 8 buses is plenty here */
        for (int dev = 0; dev < 32; dev++) {
            pci_dev_t d;
            if (!read_dev((u8)bus, (u8)dev, 0, &d)) continue;
            cb(&d); n++;
            /* multi-function device: probe the remaining functions */
            if (pci_read16((u8)bus, (u8)dev, 0, 0x0E) & 0x80) {
                for (int fn = 1; fn < 8; fn++) {
                    if (!read_dev((u8)bus, (u8)dev, (u8)fn, &d)) continue;
                    cb(&d); n++;
                }
            }
        }
    }
    return n;
}

int pci_find(u16 vendor, u16 device, pci_dev_t *out) {
    for (int bus = 0; bus < 8; bus++) {
        for (int dev = 0; dev < 32; dev++) {
            pci_dev_t d;
            if (!read_dev((u8)bus, (u8)dev, 0, &d)) continue;
            if (d.vendor == vendor && (device == 0xFFFF || d.device == device)) {
                *out = d; return 1;
            }
            if (pci_read16((u8)bus, (u8)dev, 0, 0x0E) & 0x80) {
                for (int fn = 1; fn < 8; fn++) {
                    if (!read_dev((u8)bus, (u8)dev, (u8)fn, &d)) continue;
                    if (d.vendor == vendor && (device == 0xFFFF || d.device == device)) {
                        *out = d; return 1;
                    }
                }
            }
        }
    }
    return 0;
}

/* Turn on I/O space decoding, memory space and bus mastering.  Firmware
 * usually does this already, but a device whose BARs are not enabled reads
 * back 0xFF on every port access - which looks exactly like a hung card. */
void pci_enable(const pci_dev_t *d) {
    u32 cmd = pci_read32(d->bus, d->dev, d->fn, 0x04);
    cmd |= 0x0007;                     /* I/O | memory | bus master */
    pci_write32(d->bus, d->dev, d->fn, 0x04, cmd);
}

u32 pci_bar_io(const pci_dev_t *d, int idx) {
    if (idx < 0 || idx >= 6) return 0;
    u32 bar = d->bar[idx];
    if (!(bar & 1)) return 0;              /* memory-space BAR */
    return bar & 0xFFFC;                   /* I/O base port */
}
