/* NovaOS - FAT12 filesystem reader (data disk)
 * Supports: mount (parse BPB), list the root directory, read a file by
 * following the FAT cluster chain.  Filenames are 8.3 (case-insensitive,
 * matched against the uppercase stored names).
 */
#ifndef NOVA_FAT_H
#define NOVA_FAT_H

#include "common.h"

#define FAT_MAX_FILES 32
#define FAT_NAME_LEN  16

int  fat_mount(void);                      /* 1 = ok */
int  fat_mounted(void);                    /* 1 = a volume is mounted */
int  fat_list(char names[][FAT_NAME_LEN], u32 sizes[], u32 max, u32 *count);
int  fat_read(const char *name, u8 *out, u32 max, u32 *size);

/* write side (FAT12) */
int  fat_write(const char *name, const u8 *data, u32 len);  /* create/replace */
int  fat_delete(const char *name);                          /* 1 = removed */
int  fat_stat(const char *name, u32 *size, u16 *cluster,
              u8 *attr);                                    /* 1 = found */
u32  fat_free_bytes(void);                                  /* free clusters*spc*512 */

#endif
