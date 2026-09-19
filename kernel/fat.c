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
    /* FAT12 entry: 12 bits at byte offset cluster + cluster/2 */
    u32 offset = cluster + (cluster / 2);
    u32 lba = fat_lba + (offset / 512);
    u32 off = offset % 512;
    if (!ata_read_sector(lba, secbuf)) return 0x0FFF;
    u16 v = (u16)(secbuf[off] | (secbuf[off + 1] << 8));
    if (cluster & 1) v >>= 4;
    else             v &= 0x0FFF;
    return v;
}

/* normalize "name" to the FAT 8.3 uppercase form ("hello.elf" -> "HELLO ELF") */
static void norm_name(const char *in, char out[12]) {
    char base[9] = {0}, ext[4] = {0};
    u32 i = 0;
    while (in[i] && in[i] != '.' && i < 8) {
        base[i] = (char)((in[i] >= 'a' && in[i] <= 'z') ? in[i] - 32 : in[i]);
        i++;
    }
    if (in[i] == '.') {
        i++;
        u32 j = 0;
        while (in[i] && j < 3) {
            ext[j] = (char)((in[i] >= 'a' && in[i] <= 'z') ? in[i] - 32 : in[i]);
            i++; j++;
        }
    }
    for (i = 0; i < 8; i++) out[i] = base[i];
    for (i = 0; i < 3; i++) out[8 + i] = ext[i];
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

int fat_list(char names[][FAT_NAME_LEN], u32 sizes[], u32 max, u32 *count) {
    u32 n = 0;
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

    /* follow the cluster chain */
    u32 read = 0;
    while (cluster >= 2 && read < fsize && read < max) {
        u32 lba = data_lba + (cluster - 2) * spc;
        u8  buf[512];
        if (!ata_read_sector(lba, buf)) return 0;
        u32 n = fsize - read;
        if (n > 512) n = 512;
        if (n > max - read) n = max - read;
        memcpy(out + read, buf, n);
        read += n;
        cluster = fat_next_cluster(cluster);
        if (cluster >= 0xFF0) break;        /* EOC */
    }
    *size = fsize;
    return 1;
}
