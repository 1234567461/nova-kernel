/* NovaOS - FAT12 filesystem reader */
#include "fat.h"
#include "ata.h"
#include "string.h"
#include "printf.h"
#include "serial.h"

struct fat_bpb {
    u8  jmp[3];
    u8  oem[8];
    u16 bytes_per_sector;
    u8  sectors_per_cluster;
    u16 reserved_sectors;
    u8  num_fats;
    u16 root_entries;
    u16 total_sectors16;
    u8  media;
    u16 fat_size16;
    u16 sectors_per_track;
    u16 num_heads;
    u32 hidden_sectors;
    u32 total_sectors32;
} __attribute__((packed));

static int  mounted = 0;
static u32  root_lba, data_lba;
static u32  root_sectors;
static u32  fat_lba, fat_sectors;
static u32  spc;                              /* sectors per cluster */

static u8   secbuf[512];

static u16 fat_next_cluster(u32 cluster) {
    /* FAT12 entry: 12 bits at byte offset cluster + cluster/2.
     *
     * The two bytes can straddle a sector boundary, so read the sector that
     * holds the first byte and - when the entry is split - also the one after
     * it.  Reading only secbuf[off+1] used to return garbage for every entry
     * that fell in the last byte of a sector, which silently truncated any
     * file whose cluster chain touched that slot. */
    u32 offset = cluster + (cluster / 2);
    u32 lba = fat_lba + (offset / 512);
    u32 off = offset % 512;
    if (!ata_read_sector(lba, secbuf)) return 0x0FFF;

    u16 lo, hi;
    if (off == 511) {
        lo = secbuf[511];
        if (!ata_read_sector(lba + 1, secbuf)) return 0x0FFF;
        hi = secbuf[0];
    } else {
        lo = secbuf[off];
        hi = secbuf[off + 1];
    }
    u16 v = (u16)(lo | (hi << 8));
    if (cluster & 1) v >>= 4;
    else             v &= 0x0FFF;
    return v;
}

/* normalize "name" to the FAT 8.3 uppercase form ("hello.elf" -> "HELLO   ELF").
 *
 * The directory entry is always 11 bytes wide and space (0x20) padded, so
 * the comparison buffer must be space padded too.  Zero padding never
 * matches anything on a real FAT volume - that was why "hello.elf" was
 * reported as missing even on a correctly mounted disk.
 */
static void norm_name(const char *in, char out[12]) {
    char base[8], ext[3];
    for (int k = 0; k < 8; k++) base[k] = ' ';
    for (int k = 0; k < 3; k++) ext[k]  = ' ';

    u32 i = 0;
    for (u32 k = 0; in[i] && in[i] != '.' && k < 8; k++, i++)
        base[k] = (char)((in[i] >= 'a' && in[i] <= 'z') ? in[i] - 32 : in[i]);

    if (in[i] == '.') {
        i++;
        for (u32 k = 0; in[i] && k < 3; k++, i++)
            ext[k] = (char)((in[i] >= 'a' && in[i] <= 'z') ? in[i] - 32 : in[i]);
    }

    for (u32 k = 0; k < 8; k++) out[k]     = base[k];
    for (u32 k = 0; k < 3; k++) out[8 + k] = ext[k];
    out[11] = '\0';
}

static int dir_match(const u8 *dir, const char norm[12]) {
    for (int i = 0; i < 11; i++)
        if (dir[i] != (u8)norm[i]) return 0;
    return 1;
}

int fat_mount(void) {
    mounted = 0;
    if (!ata_init()) return 0;
    if (!ata_read_sector(0, secbuf)) return 0;

    struct fat_bpb *b = (struct fat_bpb*)secbuf;
    if (b->bytes_per_sector != 512) return 0;

    fat_lba      = b->reserved_sectors;
    fat_sectors  = b->fat_size16;
    root_lba     = fat_lba + (u32)b->num_fats * fat_sectors;
    root_sectors = ((u32)b->root_entries * 32 + 511) / 512;
    data_lba     = root_lba + root_sectors;
    spc          = b->sectors_per_cluster ? b->sectors_per_cluster : 1;
    mounted      = 1;
    return 1;
}

int fat_mounted(void) { return mounted; }

int fat_list(char names[][FAT_NAME_LEN], u32 sizes[], u32 max, u32 *count) {    u32 n = 0;
    if (!mounted) return 0;
    for (u32 s = 0; s < root_sectors && n < max; s++) {
        if (!ata_read_sector(root_lba + s, secbuf)) break;
        for (u32 off = 0; off < 512 && n < max; off += 32) {
            u8 *e = secbuf + off;
            if (e[0] == 0x00) { s = root_sectors; break; }   /* end of dir */
            if (e[0] == 0xE5) continue;                       /* deleted */
            if (e[11] == 0x0F) continue;                      /* LFN entry */
            if (e[11] & 0x08) continue;                       /* volume label */
            char name[12];
            for (int i = 0; i < 8; i++) name[i] = (char)e[i];
            for (int i = 0; i < 3; i++) name[8 + i] = (char)e[8 + i];
            name[11] = '\0';
            /* strip spaces to "BASE.EXT" */
            char out[FAT_NAME_LEN];
            u32 w = 0;
            for (int i = 0; i < 8 && name[i] != ' '; i++) out[w++] = name[i];
            if (name[8] != ' ') {
                out[w++] = '.';
                for (int i = 8; i < 11 && name[i] != ' '; i++) out[w++] = name[i];
            }
            out[w] = '\0';
            strncpy(names[n], out, FAT_NAME_LEN - 1);
            names[n][FAT_NAME_LEN - 1] = '\0';
            sizes[n] = (u32)(e[28] | (e[29] << 8) | (e[30] << 16) | (e[31] << 24));
            n++;
        }
    }
    *count = n;
    return 1;
}

int fat_read(const char *name, u8 *out, u32 max, u32 *size) {
    if (!mounted) return 0;
    char norm[12];
    norm_name(name, norm);

    /* locate the directory entry */
    u32 cluster = 0, fsize = 0, found = 0;
    for (u32 s = 0; s < root_sectors && !found; s++) {
        if (!ata_read_sector(root_lba + s, secbuf)) return 0;
        for (u32 off = 0; off < 512; off += 32) {
            u8 *e = secbuf + off;
            if (e[0] == 0x00) { s = root_sectors; break; }
            if (e[0] == 0xE5 || e[11] == 0x0F) continue;
            if (dir_match(e, norm)) {
                cluster = (u32)(e[26] | (e[27] << 8));
                fsize   = (u32)(e[28] | (e[29] << 8) | (e[30] << 16) | (e[31] << 24));
                found = 1;
                break;
            }
        }
    }
    if (!found) return 0;

    /* follow the cluster chain.
     *
     * A cluster is spc sectors, not one: walking the chain one sector at a
     * time silently skipped (spc-1)/spc of every file as soon as the volume
     * used anything but 1 sector per cluster.  Read the whole cluster and
     * copy out only the bytes that are still missing. */
    u32 read = 0;
    u32 guard = 0;
    while (cluster >= 2 && cluster < 0xFF0 && read < fsize && read < max) {
        if (++guard > 65536) break;             /* corrupt chain: bail out */
        u32 first = data_lba + (cluster - 2) * spc;
        for (u32 s = 0; s < spc && read < fsize && read < max; s++) {
            u8 buf[512];
            if (!ata_read_sector(first + s, buf)) return 0;
            u32 n = fsize - read;
            if (n > 512) n = 512;
            if (n > max - read) n = max - read;
            memcpy(out + read, buf, n);
            read += n;
        }
        cluster = fat_next_cluster(cluster);
    }
    *size = read < fsize ? read : fsize;
    return 1;
}

/* ================================================================== */
/* write side                                                         */
/*                                                                    */
/* The volume has num_fats FAT copies; every update is applied to all */
/* of them so the disk stays consistent if one is ever cross-checked. */
/* ================================================================== */

static u32 data_clusters_max(void) {
    /* clusters are numbered from 2 and live after the root directory */
    u32 total_sectors = 2880;                    /* 1.44MB floppy image */
    u32 data_sectors  = total_sectors - data_lba;
    return data_sectors / (spc ? spc : 1);
}

static u32 fat_get(u32 cluster) {
    return fat_next_cluster(cluster);
}

/* write one 12-bit entry into every FAT copy */
static int fat_set(u32 cluster, u16 value) {
    u32 offset = cluster + (cluster / 2);
    u32 sec    = offset / 512;
    u32 off    = offset % 512;

    for (u32 f = 0; f < 2; f++) {                /* num_fats == 2 */
        u32 lba = fat_lba + f * fat_sectors + sec;
        if (!ata_read_sector(lba, secbuf)) return 0;

        if (off == 511) {
            /* entry straddles the sector boundary: patch the last byte here
             * and the first byte of the next sector of the same FAT copy */
            if (cluster & 1) {
                secbuf[511] = (u8)((secbuf[511] & 0x0F) | ((value << 4) & 0xF0));
            } else {
                secbuf[511] = (u8)(value & 0xFF);
            }
            if (!ata_write_sector(lba, secbuf)) return 0;
            if (!ata_read_sector(lba + 1, secbuf)) return 0;
            if (cluster & 1) {
                secbuf[0] = (u8)((value >> 4) & 0xFF);
            } else {
                secbuf[0] = (u8)((secbuf[0] & 0xF0) | ((value >> 8) & 0x0F));
            }
            if (!ata_write_sector(lba + 1, secbuf)) return 0;
        } else {
            if (cluster & 1) {
                secbuf[off]     = (u8)((secbuf[off] & 0x0F) | ((value << 4) & 0xF0));
                secbuf[off + 1] = (u8)((value >> 4) & 0xFF);
            } else {
                secbuf[off]     = (u8)(value & 0xFF);
                secbuf[off + 1] = (u8)((secbuf[off + 1] & 0xF0) | ((value >> 8) & 0x0F));
            }
            if (!ata_write_sector(lba, secbuf)) return 0;
        }
    }
    return 1;
}

/* find a free cluster (FAT entry == 0) and mark it end-of-chain */
static u32 fat_alloc_cluster(void) {
    u32 maxc = data_clusters_max();
    for (u32 c = 2; c < maxc + 2; c++) {
        if (fat_get(c) == 0) {
            if (!fat_set(c, 0x0FFF)) return 0;
            /* zero the cluster so a partially written file never exposes
             * whatever used to live on those sectors */
            for (u32 s = 0; s < spc; s++) {
                for (int i = 0; i < 512; i++) secbuf[i] = 0;
                if (!ata_write_sector(data_lba + (c - 2) * spc + s, secbuf)) return 0;
            }
            return c;
        }
    }
    return 0;                                    /* disk full */
}

/* release an entire chain back to the FAT */
static void fat_free_chain(u32 cluster) {
    u32 guard = 0;
    while (cluster >= 2 && cluster < 0xFF0) {
        if (++guard > 65536) break;
        u32 next = fat_get(cluster);
        fat_set(cluster, 0);
        cluster = next;
    }
}

u32 fat_free_bytes(void) {
    if (!mounted) return 0;
    u32 maxc = data_clusters_max();
    u32 freec = 0;
    for (u32 c = 2; c < maxc + 2; c++)
        if (fat_get(c) == 0) freec++;
    return freec * spc * 512;
}

int fat_stat(const char *name, u32 *size, u16 *cluster, u8 *attr) {
    if (!mounted) return 0;
    char norm[12];
    norm_name(name, norm);
    for (u32 s = 0; s < root_sectors; s++) {
        if (!ata_read_sector(root_lba + s, secbuf)) return 0;
        for (u32 off = 0; off < 512; off += 32) {
            u8 *e = secbuf + off;
            if (e[0] == 0x00) return 0;
            if (e[0] == 0xE5 || e[11] == 0x0F) continue;
            if (dir_match(e, norm)) {
                if (size)    *size    = (u32)(e[28] | (e[29] << 8) |
                                              (e[30] << 16) | (e[31] << 24));
                if (cluster) *cluster = (u16)(e[26] | (e[27] << 8));
                if (attr)    *attr    = e[11];
                return 1;
            }
        }
    }
    return 0;
}

/* locate the directory slot for `norm`; returns sector+offset via out params.
 * If it does not exist, returns the first free slot instead so a create can
 * reuse it.  `existed` reports which of the two happened. */
static int dir_find_slot(const char norm[12], u32 *out_lba, u32 *out_off,
                         int *existed) {
    u32 first_free_lba = 0, first_free_off = 0;
    int have_free = 0;

    for (u32 s = 0; s < root_sectors; s++) {
        u32 lba = root_lba + s;
        if (!ata_read_sector(lba, secbuf)) return 0;
        for (u32 off = 0; off < 512; off += 32) {
            u8 *e = secbuf + off;
            if (e[0] == 0xE5 || e[0] == 0x00) {
                if (!have_free) {
                    first_free_lba = lba;
                    first_free_off = off;
                    have_free = 1;
                }
                if (e[0] == 0x00) {
                    /* end of directory: no match can follow */
                    *out_lba = first_free_lba;
                    *out_off = first_free_off;
                    *existed = 0;
                    return have_free;
                }
                continue;
            }
            if (e[11] == 0x0F) continue;             /* LFN entry */
            if (dir_match(e, norm)) {
                *out_lba = lba;
                *out_off = off;
                *existed = 1;
                return 1;
            }
        }
    }
    if (have_free) {
        *out_lba = first_free_lba;
        *out_off = first_free_off;
        *existed = 0;
        return 1;
    }
    return 0;                                        /* directory full */
}

int fat_delete(const char *name) {
    if (!mounted) return 0;
    char norm[12];
    norm_name(name, norm);

    u32 lba, off; int existed = 0;
    if (!dir_find_slot(norm, &lba, &off, &existed) || !existed) return 0;

    if (!ata_read_sector(lba, secbuf)) return 0;
    u8 *e = secbuf + off;
    u32 cluster = (u32)(e[26] | (e[27] << 8));
    e[0] = 0xE5;                                     /* mark deleted */
    if (!ata_write_sector(lba, secbuf)) return 0;

    fat_free_chain(cluster);
    return 1;
}

int fat_write(const char *name, const u8 *data, u32 len) {
    if (!mounted) return 0;
    char norm[12];
    norm_name(name, norm);

    u32 lba, off; int existed = 0;
    if (!dir_find_slot(norm, &lba, &off, &existed)) return 0;

    /* replacing an existing file: drop the old chain first */
    if (existed) {
        if (!ata_read_sector(lba, secbuf)) return 0;
        u32 oldc = (u32)(secbuf[off + 26] | (secbuf[off + 27] << 8));
        fat_free_chain(oldc);
    }

    u32 per_cluster = spc * 512;
    u32 need = (len + per_cluster - 1) / per_cluster;

    u32 first = 0, prev = 0, written = 0;
    for (u32 i = 0; i < need; i++) {
        u32 c = fat_alloc_cluster();
        if (!c) {                                    /* out of space: unwind */
            if (first) fat_free_chain(first);
            return 0;
        }
        if (!first) first = c;
        if (prev)   fat_set(prev, (u16)c);
        prev = c;
    }

    /* stream the payload into the chain */
    u32 c = first;
    while (c >= 2 && written < len) {
        for (u32 s = 0; s < spc && written < len; s++) {
            for (int i = 0; i < 512; i++)
                secbuf[i] = (written < len) ? data[written++] : 0;
            if (!ata_write_sector(data_lba + (c - 2) * spc + s, secbuf)) {
                fat_free_chain(first);
                return 0;
            }
        }
        if (written < len) c = fat_get(c);
    }

    /* write (or create) the directory entry */
    if (!ata_read_sector(lba, secbuf)) return 0;
    u8 *e = secbuf + off;
    for (int i = 0; i < 11; i++) e[i] = (u8)norm[i];
    e[11] = 0x20;                                    /* archive */
    e[12] = 0;                                       /* reserved */
    e[13] = 0;                                       /* creation time tenths */
    for (int i = 14; i < 26; i++) e[i] = 0;          /* times / dates */
    e[26] = (u8)(first & 0xFF);
    e[27] = (u8)((first >> 8) & 0xFF);
    e[28] = (u8)(len & 0xFF);
    e[29] = (u8)((len >> 8) & 0xFF);
    e[30] = (u8)((len >> 16) & 0xFF);
    e[31] = (u8)((len >> 24) & 0xFF);
    if (!ata_write_sector(lba, secbuf)) return 0;
    return 1;
}
