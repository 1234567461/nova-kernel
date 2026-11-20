/* NovaOS - ATA PIO driver (LBA28, auto-probing)
 *
 * The data disk can be attached in several places depending on the
 * environment:
 *   QEMU test rigs  -drive index=2   -> secondary master (0x170, drive 0)
 *   v86 (browser)   hda              -> primary master   (0x1F0, drive 0)
 * and v86 0.5.x hard-codes the secondary master as an ATAPI CD-ROM,
 * silently ignoring the "hdc" setting, so we cannot rely on one fixed
 * location.  Probe all four positions and keep the first device that
 * answers and is not ATAPI (LBA mid/high signature 0x14/0xEB).
 */
#ifndef NOVA_ATA_H
#define NOVA_ATA_H

#include "common.h"

#define ATA_PRIMARY   0x1F0u
#define ATA_SECONDARY 0x170u

int  ata_init(void);                 /* probe; 1 = drive present */
int  ata_read_sector(u32 lba, u8 *buf);   /* 1 on success */
int  ata_write_sector(u32 lba, const u8 *buf);  /* 1 on success */

#endif
