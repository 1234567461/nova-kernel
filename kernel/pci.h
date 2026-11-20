/* NovaOS - PCI bus enumeration (config space via 0xCF8/0xCFC) */
#ifndef NOVA_PCI_H
#define NOVA_PCI_H

#include "common.h"

#define PCI_VENDOR_REALTEK 0x10EC

typedef struct {
    u8  bus, dev, fn;
    u16 vendor, device;
    u8  class, subclass, prog_if;
    u8  irq;
    u32 bar[6];
} pci_dev_t;

void  pci_init(void);
/* scan every bus/device/function, call cb for each present device */
int   pci_scan(void (*cb)(const pci_dev_t *d));
int   pci_find(u16 vendor, u16 device, pci_dev_t *out);
u32   pci_read32(u8 bus, u8 dev, u8 fn, u8 off);
u16   pci_read16(u8 bus, u8 dev, u8 fn, u8 off);
void  pci_write32(u8 bus, u8 dev, u8 fn, u8 off, u32 val);
u32   pci_bar_io(const pci_dev_t *d, int idx);   /* I/O-space BAR (port) or 0 */
void  pci_enable(const pci_dev_t *d);            /* enable I/O + memory + bus master */

#endif
