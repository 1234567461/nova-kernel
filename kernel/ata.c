/* NovaOS - ATA PIO driver, LBA28, auto-probing both IDE channels.
 *
 * History: this driver used to assume the data disk was the secondary
 * master because that is what the QEMU test rig attaches.  Browsers use
 * v86, whose IDE configuration hard-codes the secondary master as an
 * ATAPI CD-ROM and ignores the "hdc" setting entirely - the probe then
 * saw a device, but every READ SECTORS was aborted with the ATAPI
 * signature and fat_mount() failed with "sector 0 read failed".
 *
 * We now probe: primary master -> primary slave -> secondary master ->
 * secondary slave, skip ATAPI devices (LBA mid/high = 0x14/0xEB) and
 * keep the first plain ATA disk.  Both test environments work without
 * any configuration on the caller's side.
 */
#include "ata.h"
#include "io.h"
#include "serial.h"
#include "printf.h"

static int ata_present = 0;
static u16 ata_base    = 0;      /* port base of the selected channel */
static u8  ata_drive   = 0;      /* 0xE0 master, 0xF0 slave (LBA) */

static void ata_poll_busy(u16 base) {
    for (u32 i = 0; i < 2000000; i++) {
        if (!(inb(base + 7) & 0x80)) return;   /* BSY clear */
    }
}

static void ata_select(u16 base, u8 drive) {
    outb(base + 6, drive);
    io_wait();
    for (int i = 0; i < 4; i++) (void)inb(base + 7);   /* 400ns settle */
}

/* 1 = a plain (non-ATAPI) ATA device answers at base/drive */
static int ata_probe(u16 base, u8 drive) {
    ata_select(base, drive);
    u8 st = inb(base + 7);
    if (st == 0xFF || st == 0x00) return 0;        /* floating bus / none */
    u8 mid  = inb(base + 4);
    u8 high = inb(base + 5);
    if (mid == 0x14 && high == 0xEB) return 0;     /* ATAPI: not our disk */
    return 1;
}

int ata_init(void) {
    static const u16 bases[2] = { ATA_PRIMARY, ATA_SECONDARY };
    static const u8  drives[2] = { 0xE0, 0xF0 };   /* bit6 = LBA mode */

    if (ata_present) return 1;      /* already probed (fat_mount calls us) */
    ata_present = 0;

    for (int b = 0; b < 2; b++) {
        for (int d = 0; d < 2; d++) {
            if (!ata_probe(bases[b], drives[d])) continue;
            ata_base  = bases[b];
            ata_drive = drives[d];
            ata_present = 1;
            kprintf("ata: %s %s (0x%x)\n",
                    b == 0 ? "primary" : "secondary",
                    d == 0 ? "master" : "slave",
                    (u32)bases[b]);
            return 1;
        }
    }
    return 0;
}

int ata_read_sector(u32 lba, u8 *buf) {
    if (!ata_present) return 0;
    ata_select(ata_base, (u8)(ata_drive | ((lba >> 24) & 0x0F)));
    outb(ata_base + 2, 1);                      /* sector count */
    outb(ata_base + 3, (u8)(lba & 0xFF));       /* LBA low */
    outb(ata_base + 4, (u8)((lba >> 8) & 0xFF));
    outb(ata_base + 5, (u8)((lba >> 16) & 0xFF));
    outb(ata_base + 7, 0x20);                   /* READ SECTORS */
    ata_poll_busy(ata_base);
    if (inb(ata_base + 7) & 0x01) return 0;     /* error bit */
    __asm__ volatile (
        "cld\n\t"
        "rep insw\n\t"
        : : "D"(buf), "c"(256), "d"(ata_base) : "memory");
    return 1;
}

int ata_write_sector(u32 lba, const u8 *buf) {
    if (!ata_present) return 0;
    ata_poll_busy(ata_base);
    /* wait until the drive is ready to accept a command (DRDY, not busy) */
    for (u32 i = 0; i < 2000000; i++) {
        u8 st = inb(ata_base + 7);
        if (!(st & 0x80) && (st & 0x40)) break;      /* !BSY && DRDY */
    }

    ata_select(ata_base, (u8)(ata_drive | ((lba >> 24) & 0x0F)));
    outb(ata_base + 2, 1);                      /* sector count */
    outb(ata_base + 3, (u8)(lba & 0xFF));
    outb(ata_base + 4, (u8)((lba >> 8) & 0xFF));
    outb(ata_base + 5, (u8)((lba >> 16) & 0xFF));
    outb(ata_base + 7, 0x30);                   /* WRITE SECTORS */

    /* one sector = 256 words; the device wants them as fast as we can feed */
    __asm__ volatile (
        "cld\n\t"
        "rep outsw\n\t"
        : : "S"(buf), "c"(256), "d"(ata_base) : "memory");

    /* flush the write cache, then confirm the command completed cleanly */
    outb(ata_base + 7, 0xE7);                   /* FLUSH CACHE */
    ata_poll_busy(ata_base);
    if (inb(ata_base + 7) & 0x01) return 0;     /* ERR */
    return 1;
}
