/* NovaOS - ATA PIO driver (secondary master, LBA28) */
#include "ata.h"
#include "io.h"
#include "serial.h"

static int ata_present = 0;

static void ata_poll_busy(void) {
    for (u32 i = 0; i < 2000000; i++) {
        if (!(inb(ATA_SECONDARY + 7) & 0x80)) return;   /* BSY clear */
    }
}

int ata_init(void) {
    outb(ATA_SECONDARY + 6, 0xE0);   /* select master, LBA mode */
    io_wait();
    ata_poll_busy();
    u8 st = inb(ATA_SECONDARY + 7);
    ata_present = (st != 0xFF);      /* 0xFF means no device on channel */
    return ata_present;
}

int ata_read_sector(u32 lba, u8 *buf) {
    if (!ata_present) return 0;
    outb(ATA_SECONDARY + 6, (u8)(0xE0 | ((lba >> 24) & 0x0F)));
    outb(ATA_SECONDARY + 2, 1);                      /* sector count */
    outb(ATA_SECONDARY + 3, (u8)(lba & 0xFF));       /* LBA low */
    outb(ATA_SECONDARY + 4, (u8)((lba >> 8) & 0xFF));
    outb(ATA_SECONDARY + 5, (u8)((lba >> 16) & 0xFF));
    outb(ATA_SECONDARY + 7, 0x20);                   /* READ SECTORS */
    ata_poll_busy();
    if (inb(ATA_SECONDARY + 7) & 0x01) return 0;     /* error bit */
    __asm__ volatile (
        "cld\n\t"
        "rep insw\n\t"
        : : "D"(buf), "c"(256), "d"(ATA_SECONDARY) : "memory");
    return 1;
}
