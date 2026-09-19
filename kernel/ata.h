/* NovaOS - ATA PIO driver (secondary master, LBA28)
 * The data disk (data.img) is attached as the second floppy in QEMU
 * (-fdb build/data.img), which the emulator exposes as the secondary
 * IDE channel.  This driver reads raw 512-byte sectors from it.
 */
#ifndef NOVA_ATA_H
#define NOVA_ATA_H

#include "common.h"

#define ATA_SECONDARY 0x170u

int  ata_init(void);                 /* probe; 1 = drive present */
int  ata_read_sector(u32 lba, u8 *buf);   /* 1 on success */

#endif
