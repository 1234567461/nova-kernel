/* ============================================================================
 *  Nova-Linux 1.0 (Sun) - graphical install wizard
 *
 *  Self-contained: draws on /dev/fb0 (bochs/virtio-gpu/qxl DRM fbdev),
 *  reads keyboard + mouse/touch from /dev/input/event* (evdev is built-in).
 *  No X, no external toolkit, no typing needed - everything is selectable
 *  with arrows + Enter, mouse click, or touch tap.
 *
 *  Modes:
 *    1) Erase entire disk & install      (full-disk wipe)
 *    2) Install alongside existing OS    (uses unpartitioned free space,
 *                                         existing bootloader untouched;
 *                                         chainload/update-grub afterwards)
 *    3) Live shell                       (exit to BusyBox, run novainstall)
 * ========================================================================== */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <linux/fb.h>
#include <linux/input.h>
#include "font.h"

/* ---------------------------------------------------------------- colors */
#define C_BG      0x1E1E2E
#define C_PANEL   0x2A2A3C
#define C_ACCENT  0xE95420   /* Ubuntu orange */
#define C_OK      0x4CAF50
#define C_TEXT    0xFFFFFF
#define C_DIM     0x9AA0B5
#define C_SEL     0xE95420
#define C_WARN    0xFFB74D

/* ------------------------------------------------------------------- fb */
static int fbfd = -1;
static unsigned char *fb = NULL;
static int W = 0, H = 0, BPP = 0, ROWL = 0;

static void px(int x, int y, uint32_t c) {
    if (x < 0 || y < 0 || x >= W || y >= H) return;
    unsigned char *p = fb + (size_t)y * ROWL + x * (BPP / 8);
    if (BPP == 32) { *(uint32_t *)p = c; }
    else if (BPP == 24) { p[0] = c & 0xFF; p[1] = (c >> 8) & 0xFF; p[2] = (c >> 16) & 0xFF; }
    else if (BPP == 16) { *(uint16_t *)p = ((c >> 3) & 0xF800) | ((c >> 5) & 0x07E0) | ((c >> 8) & 0x1F); }
}

static void rect(int x, int y, int w, int h, uint32_t c) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) px(x + i, y + j, c);
}

static void text(int x, int y, const char *s, uint32_t c, int scale) {
    for (; *s; s++, x += FONT_W * scale) {
        if ((unsigned char)*s > 255) continue;
        const unsigned char *g = font_bitmap + (size_t)(*s) * FONT_H;
        for (int row = 0; row < FONT_H; row++) {
            unsigned char bits = g[row];
            for (int col = 0; col < 8; col++)
                if (bits & (0x80 >> col))
                    for (int sy = 0; sy < scale; sy++)
                        for (int sx = 0; sx < scale; sx++)
                            px(x + col * scale + sx, y + row * scale + sy, c);
        }
    }
}

static void center(int y, const char *s, uint32_t c, int scale) {
    text((W - (int)strlen(s) * FONT_W * scale) / 2, y, s, c, scale);
}

/* ------------------------------------------------------------------ ui */
#define MAXBTN 8
typedef struct { int x, y, w, h; const char *label; int id; } Btn;
/* Resolution-independent layout: the installer must fill whatever screen it
 * is given (a 4K panel must not show a stamp-sized dialog).  FS is the font
 * scale and every position below is expressed as a fraction of W/H. */
static int FS = 2;
static int vw(int pct) { return W * pct / 100; }
static int vh(int pct) { return H * pct / 100; }
static int pxs(int px) { return px * FS / 2; }   /* design px @FS=2 -> screen */

static Btn btns[MAXBTN];
static int nbtn = 0, sel = 0, mx = -100, my = -100;

static void btn_clear(void) { nbtn = 0; sel = 0; }
static void btn_add(int x, int y, int w, int h, const char *label, int id) {
    if (nbtn >= MAXBTN) return;
    btns[nbtn++] = (Btn){ x, y, w, h, label, id };
}
static void draw_btns(void) {
    for (int i = 0; i < nbtn; i++) {
        Btn *b = &btns[i];
        int on = (i == sel);
        int tw = (int)strlen(b->label) * FONT_W * FS;
        if (tw + 10 * FS > b->w) b->w = tw + 10 * FS;   /* never clip a label */
        int pad = FS;
        rect(b->x - pad, b->y - pad, b->w + 2 * pad, b->h + 2 * pad, on ? C_ACCENT : C_PANEL);
        rect(b->x, b->y, b->w, b->h, on ? 0x3A2A20 : C_PANEL);
        text(b->x + (b->w - tw) / 2, b->y + (b->h - FONT_H * FS) / 2, b->label,
             on ? C_ACCENT : C_TEXT, FS);
    }
}
static void draw_cursor(void) {
    for (int i = -6; i <= 6; i++) {
        px(mx + i, my, C_TEXT); px(mx, my + i, C_TEXT);
    }
    px(mx, my, C_ACCENT);
}
static void flush(void) { if (mx >= 0) draw_cursor(); }

/* --------------------------------------------------------------- system */
static int step_run(const char *cmd) {
    FILE *l = fopen("/tmp/inst.log", "a");
    if (l) { fprintf(l, "$ %s\n", cmd); fclose(l); }
    char buf[1024];
    snprintf(buf, sizeof buf, "%s >>/tmp/inst.log 2>&1", cmd);
    return system(buf);
}
static void log_line(const char *s) {
    FILE *l = fopen("/tmp/inst.log", "a");
    if (l) { fprintf(l, "%s\n", s); fclose(l); }
}
/* read last n lines of the install log into buf (newline separated) */
static void tail_log(char *buf, int bufsz, int nlines) {
    buf[0] = 0;
    FILE *f = fopen("/tmp/inst.log", "r");
    if (!f) return;
    static char lines[16][160]; int n = 0;
    while (fgets(lines[n % 16], 160, f)) n++;
    fclose(f);
    int start = n > nlines ? n - nlines : 0;
    int off = 0;
    for (int i = start; i < n && off < bufsz - 170; i++) {
        char *s = lines[i % 16];
        size_t len = strlen(s);
        if (len && s[len - 1] == '\n') s[len - 1] = 0;
        off += snprintf(buf + off, bufsz - off, "%s\n", s);
    }
}

/* --------------------------------------------------------------- disks */
#define MAXDISK 8
typedef struct { char dev[32]; long mb; } Disk;
static Disk disks[MAXDISK];
static int ndisk = 0;

static void scan_disks(void) {
    ndisk = 0;
    DIR *d = opendir("/sys/block");
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && ndisk < MAXDISK) {
        if (e->d_name[0] == '.') continue;
        if (!strncmp(e->d_name, "loop", 4) || !strncmp(e->d_name, "ram", 3) ||
            !strncmp(e->d_name, "sr", 2) || !strncmp(e->d_name, "zram", 4)) continue;
        char p[128];
        snprintf(p, sizeof p, "/sys/block/%s/size", e->d_name);
        FILE *f = fopen(p, "r");
        if (!f) continue;
        long sec = 0;
        if (fscanf(f, "%ld", &sec) != 1 || sec <= 0) { fclose(f); continue; }
        fclose(f);
        snprintf(disks[ndisk].dev, sizeof disks[ndisk].dev, "%s", e->d_name);
        disks[ndisk].mb = sec * 512 / 1024 / 1024;
        ndisk++;
    }
    closedir(d);
}

/* --------------------------------------------------- free-space analysis */
/* returns 0 and fills start/size (sectors) when unpartitioned space exists */
static int find_free(const char *dev, long *fstart, long *fsize, int *gpt) {
    char cmd[256], line[256];
    snprintf(cmd, sizeof cmd, "sfdisk -d /dev/%s 2>/dev/null", dev);
    FILE *f = popen(cmd, "r");
    if (!f) return -1;
    *gpt = 0;
    long lo[64], hi[64]; int n = 0, npart = 0;
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, "label: gpt", 10)) *gpt = 1;
        char *s = strstr(line, "start=");
        if (!s) continue;
        long st = 0, sz = 0;
        if (sscanf(s, "start=%ld, size=%ld", &st, &sz) == 2) {
            if (n < 64 && sz > 0) { lo[n] = st; hi[n] = st + sz; n++; }
            npart++;
        }
    }
    pclose(f);
    snprintf(cmd, sizeof cmd, "/sys/block/%s/size", dev);
    f = fopen(cmd, "r");
    if (!f) return -1;
    long total = 0;
    if (fscanf(f, "%ld", &total) != 1) { fclose(f); return -1; }
    fclose(f);
    /* sort intervals */
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            if (lo[j] < lo[i]) {
                long t; t = lo[i]; lo[i] = lo[j]; lo[j] = t;
                t = hi[i]; hi[i] = hi[j]; hi[j] = t;
            }
    long cur = 2048;                 /* 1MB alignment */
    for (int i = 0; i < n; i++) {
        if (hi[i] <= cur) continue;
        if (lo[i] - cur >= 4096) { *fstart = cur; *fsize = lo[i] - cur - 2048; return 0; }
        if (hi[i] > cur) cur = hi[i];
    }
    if (total - cur >= 4096) { *fstart = cur; *fsize = total - cur - 2048; return 0; }
    (void)npart;
    return -1;
}

/* ------------------------------------------------------------ installer */
static int gui_erase(const char *dev, char *err, int errsz) {
    char cmd[512];
    snprintf(cmd, sizeof cmd, "printf 'o\\nn\\np\\n1\\n\\n\\nw\\n' | fdisk /dev/%s >/dev/null 2>&1", dev);
    if (step_run(cmd) != 0) { snprintf(err, errsz, "fdisk failed"); return -1; }
    sleep(1); step_run("mdev -s 2>/dev/null");
    snprintf(cmd, sizeof cmd, "mke2fs -F /dev/%s1", dev);
    if (step_run(cmd) != 0) { snprintf(err, errsz, "mkfs failed"); return -1; }
    snprintf(cmd, sizeof cmd, "mkdir -p /mnt/target && mount -t ext2 /dev/%s1 /mnt/target", dev);
    step_run(cmd);
    if (step_run("cp -a /rootfs-template/. /mnt/target/") != 0) { snprintf(err, errsz, "copy system failed"); return -1; }
    step_run("mkdir -p /mnt/target/boot/extlinux && cp /usr/lib/syslinux/modules/bios/ldlinux.c32 /mnt/target/boot/extlinux/");
    step_run("cp /boot/vmlinuz /mnt/target/boot/vmlinuz && cp /boot/initramfs-live.gz /mnt/target/boot/initramfs.gz");
    snprintf(cmd, sizeof cmd, "cat > /mnt/target/boot/extlinux/extlinux.conf <<'CONF'\nDEFAULT nova-linux\nTIMEOUT 30\nLABEL nova-linux\n  KERNEL /boot/vmlinuz\n  INITRD /boot/initramfs.gz\n  APPEND root=/dev/%s1 console=ttyS0 console=tty0\nCONF", dev);
    step_run(cmd);
    snprintf(cmd, sizeof cmd, "extlinux --install /mnt/target/boot/extlinux");
    step_run(cmd);
    snprintf(cmd, sizeof cmd, "dd if=/usr/lib/altmbr.bin of=/dev/%s bs=439 count=1 2>/dev/null", dev);
    step_run(cmd);
    snprintf(cmd, sizeof cmd, "printf '\\001' | dd of=/dev/%s bs=1 seek=439 conv=notrunc 2>/dev/null", dev);
    step_run(cmd);
    step_run("umount /mnt/target; sync");
    return 0;
}

static int gui_alongside(const char *dev, long fstart, long fsize, int gpt,
                         int pnum, char *err, int errsz) {
    char cmd[512];
    /* append the new partition via sfdisk dump + explicit line */
    snprintf(cmd, sizeof cmd, "sfdisk -d /dev/%s > /tmp/tbl 2>/dev/null", dev);
    step_run(cmd);
    FILE *t = fopen("/tmp/tbl", "a");
    if (!t) { snprintf(err, errsz, "sfdisk dump failed"); return -1; }
    fprintf(t, "/dev/%s%d : start=%ld, size=%ld, type=%s\n",
            dev, pnum, fstart, fsize, gpt ? "linux" : "83");
    fclose(t);
    snprintf(cmd, sizeof cmd, "sfdisk /dev/%s < /tmp/tbl", dev);
    if (step_run(cmd) != 0) { snprintf(err, errsz, "partitioning failed"); return -1; }
    sleep(1); step_run("mdev -s 2>/dev/null");
    snprintf(cmd, sizeof cmd, "mke2fs -F /dev/%s%d", dev, pnum);
    if (step_run(cmd) != 0) { snprintf(err, errsz, "mkfs failed"); return -1; }
    snprintf(cmd, sizeof cmd, "mkdir -p /mnt/target && mount -t ext2 /dev/%s%d /mnt/target", dev, pnum);
    step_run(cmd);
    if (step_run("cp -a /rootfs-template/. /mnt/target/") != 0) { snprintf(err, errsz, "copy system failed"); return -1; }
    step_run("mkdir -p /mnt/target/boot/extlinux && cp /usr/lib/syslinux/modules/bios/ldlinux.c32 /mnt/target/boot/extlinux/");
    step_run("cp /boot/vmlinuz /mnt/target/boot/vmlinuz && cp /boot/initramfs-live.gz /mnt/target/boot/initramfs.gz");
    snprintf(cmd, sizeof cmd, "cat > /mnt/target/boot/extlinux/extlinux.conf <<'CONF'\nDEFAULT nova-linux\nTIMEOUT 30\nLABEL nova-linux\n  KERNEL /boot/vmlinuz\n  INITRD /boot/initramfs.gz\n  APPEND root=/dev/%s%d console=ttyS0 console=tty0\nCONF", dev, pnum);
    step_run(cmd);
    /* dual boot: bootloader goes into the PARTITION boot sector, MBR untouched */
    snprintf(cmd, sizeof cmd, "extlinux --install /mnt/target/boot/extlinux");
    step_run(cmd);
    step_run("umount /mnt/target; sync");
    log_line("DONE: to boot it from your existing OS, run 'update-grub' there,");
    log_line("or pick the partition from your firmware boot menu.");
    return 0;
}

/* -------------------------------------------------------------- screens */
enum { SCR_MODE, SCR_DISK, SCR_CONFIRM, SCR_RUN, SCR_DONE, SCR_MSG };
static int scr = SCR_MODE;
static int mode = 0;                 /* 0 erase, 1 alongside */
static char msg[512];
static char tdev[32];
static long tfstart, tfsize;
static int tgpt, mode_pnum = 2;
static int result_ok = 0;
static int step_i = 0, step_n = 8;

#define HINT_H (7 * FS + 6)

static int btn_h(void) { return 8 * FS + 14; }
static int btn_gap(void) { return 3 * FS + 8; }

static void header(void) {
    int hh = 20 * FS + 4;
    rect(0, 0, W, hh, C_PANEL);
    rect(0, hh, W, 3, C_ACCENT);
    text(6 * FS, (hh - FONT_H * FS) / 2, "Nova-Linux 1.0 (Sun)", C_TEXT, FS);
    const char *r = "Install Wizard";
    int rw = (int)strlen(r) * FONT_W * FS;
    text(W - rw - 6 * FS, (hh - FONT_H * FS) / 2, r, C_ACCENT, FS);
}

static void scr_mode(void) {
    btn_clear();
    header();
    int bh = 9 * FS + 18, gap = btn_gap();
    int bw = vw(52); if (bw > 980) bw = 980; if (bw < 420) bw = 420;
    int bx = (W - bw) / 2;
    int by = vh(36);
    const char *items[3] = { "Erase entire disk & install",
                             "Install alongside existing OS (free space)",
                             "Live shell (no install)" };
    for (int i = 0; i < 3; i++)
        btn_add(bx, by + i * (bh + gap), bw, bh, items[i], i + 1);
    center(vh(15), "Welcome to Nova-Linux", C_TEXT, FS + 2);
    center(vh(15) + (FS + 2) * FONT_H + 5 * FS, "Choose what to do:", C_DIM, FS);
    draw_btns(); flush();
}

static void scr_disk(void) {
    btn_clear();
    header();
    center(vh(11), mode ? "Install alongside - pick target disk"
                        : "Erase & install - pick target disk", C_TEXT, FS + 1);
    int bw = vw(56); if (bw > 1000) bw = 1000; if (bw < 420) bw = 420;
    int bx = (W - bw) / 2;
    int bh = btn_h();
    if (!ndisk) {
        center(vh(40), "No disks found in this machine.", C_WARN, FS);
        btn_add(bx, H - HINT_H - bh - 6 * FS, bw, bh, "Back", 0);
        draw_btns(); flush(); return;
    }
    int by = vh(24);
    char line[96];
    for (int i = 0; i < ndisk && i < MAXBTN - 1; i++) {
        snprintf(line, sizeof line, "/dev/%s   -   %ld MB", disks[i].dev, disks[i].mb);
        btn_add(bx, by + i * (bh + btn_gap()), bw, bh, "", 10 + i);
        Btn *b = &btns[nbtn - 1];
        int on = (nbtn - 1 == sel);
        rect(b->x - FS, b->y - FS, b->w + 2 * FS, b->h + 2 * FS, on ? C_ACCENT : C_PANEL);
        rect(b->x, b->y, b->w, b->h, on ? 0x3A2A20 : C_PANEL);
        text(b->x + 6 * FS, b->y + (b->h - FONT_H * FS) / 2, line, on ? C_ACCENT : C_TEXT, FS);
    }
    btn_add(bx, H - HINT_H - bh - 6 * FS, bw, bh, "Back", 0);
    draw_btns(); flush();
}

static void scr_confirm(const char *dev, long fstart, long fsize) {
    btn_clear();
    header();
    center(vh(12), "Ready to install", C_TEXT, FS + 2);
    char line[128];
    if (mode) {
        long mb = fsize * 512 / 1024 / 1024;
        snprintf(line, sizeof line, "Mode: alongside  |  /dev/%s%d  (start %ld, %ld MB)",
                 dev, mode_pnum, fstart, mb);
    } else {
        snprintf(line, sizeof line, "Mode: ERASE ALL DATA on /dev/%s", dev);
    }
    int y = vh(30);
    center(y, line, mode ? C_TEXT : C_WARN, FS);
    center(y + FONT_H * FS + 4 * FS, mode ? "Your existing system will NOT be touched."
                                          : "Everything on this disk will be destroyed!",
           mode ? C_DIM : C_WARN, FS);
    center(y + 2 * (FONT_H * FS + 4 * FS),
           mode ? "After install: run 'update-grub' in your existing OS"
                : "Nova-Linux will own the whole disk.", C_DIM, FS - 1 < 1 ? 1 : FS - 1);
    int bw = vw(50); if (bw > 860) bw = 860; if (bw < 420) bw = 420;
    int bx = (W - bw) / 2, bh = 8 * FS + 16;
    btn_add(bx, vh(62), bw, bh, mode ? "Install now (safe)" : "ERASE & INSTALL NOW", 99);
    btn_add(bx, vh(62) + bh + btn_gap() + 4 * FS, bw, bh - 4 * FS, "Cancel", 0);
    draw_btns(); flush();
}

static void scr_run(void) {
    btn_clear();
    header();
    center(vh(13), "Installing Nova-Linux ...", C_TEXT, FS + 1);
    int bw = vw(66), bx = (W - bw) / 2, bh = 3 * FS + 10;
    rect(bx, vh(24), bw, bh, C_PANEL);
    rect(bx, vh(24), bw * step_i / step_n, bh, C_ACCENT);
    char buf[1200];
    tail_log(buf, sizeof buf, 14);
    int sc = FS - 1 < 1 ? 1 : FS - 1;
    int y = vh(34);
    for (char *t = strtok(buf, "\n"); t; t = strtok(NULL, "\n")) {
        text(6 * FS, y, t, C_DIM, sc); y += FONT_H * sc + 3;
        if (y > H - HINT_H - FONT_H * sc) break;
    }
    flush();
}

static void scr_done(int ok) {
    btn_clear();
    header();
    center(vh(18), ok ? "Installation complete!" : "Installation failed", ok ? C_OK : C_WARN, FS + 2);
    center(vh(18) + (FS + 2) * FONT_H + 5 * FS,
           ok ? "Nova-Linux is on your disk." : "See log above / live shell for details.",
           C_DIM, FS);
    int bw = vw(46); if (bw > 760) bw = 760; if (bw < 380) bw = 380;
    int bx = (W - bw) / 2, bh = 8 * FS + 16;
    int by = vh(50);
    if (ok) {
        btn_add(bx, by, bw, bh, "Reboot into Nova-Linux", 101);
        btn_add(bx, by + bh + btn_gap() + 2 * FS, bw, bh - 4 * FS, "Exit to shell", 102);
    } else {
        btn_add(bx, by, bw, bh, "Exit to shell", 102);
    }
    draw_btns(); flush();
}

static void scr_msg(void) {
    btn_clear();
    header();
    center(vh(26), msg, C_WARN, FS);
    int bw = vw(30); if (bw < 260) bw = 260;
    btn_add((W - bw) / 2, vh(50), bw, btn_h(), "OK", 0);
    draw_btns(); flush();
}

/* wrappers for uniform screen table */
static void scr_confirm_w(void) { scr_confirm(tdev, tfstart, tfsize); }
static void scr_done_w(void) { scr_done(result_ok); }
static void (*screens[])(void) = { scr_mode, scr_disk, scr_confirm_w, scr_run, scr_done_w, scr_msg };
/* --- persistent help strip: the single most common complaint was
 *     "I don't know how to choose", so it is always on screen ---------- */
static void hint_bar(void) {
    int hs = 7 * FS + 6;
    int y = H - hs;
    rect(0, y, W, hs, C_PANEL);
    rect(0, y, W, 2, C_ACCENT);
    const char *h = "UP/DN or TAB move   ENTER or SPACE choose   1/2/3 shortcuts   CLICK / TAP a box";
    int sc = FS > 1 ? FS - 1 : 1;
    int tw = (int)strlen(h) * FONT_W * sc;
    text(tw > W ? 0 : (W - tw) / 2, y + (hs - FONT_H * sc) / 2, h, C_DIM, sc);
}
static void redraw(void) {
    rect(0, 0, W, H, C_BG);
    screens[scr]();
    hint_bar();
    if (mx >= 0) draw_cursor();
}

/* ---------------------------------------------------------- transitions */
static void start_install(void) {
    unlink("/tmp/inst.log");
    scr = SCR_RUN;
    step_n = mode ? 7 : 9;
    step_i = 1; redraw();
    char err[160]; err[0] = 0;
    int rc = mode ? gui_alongside(tdev, tfstart, tfsize, tgpt, mode_pnum, err, sizeof err)
                  : gui_erase(tdev, err, sizeof err);
    step_i = step_n;
    if (rc == 0) { result_ok = 1; scr = SCR_DONE; }
    else { result_ok = 0; scr = SCR_DONE; log_line(err); }
    redraw();
}

static void activate(int id) {
    if (scr == SCR_MODE) {
        if (id == 1 || id == 2) { mode = id - 1; scr = SCR_DISK; }
        else if (id == 3) { execl("/bin/busybox", "sh", NULL); exit(0); }
    } else if (scr == SCR_DISK) {
        if (id == 0) { scr = SCR_MODE; }
        else if (id >= 10 && id < 10 + ndisk) {
            snprintf(tdev, sizeof tdev, "%s", disks[id - 10].dev);
            if (mode) {
                if (find_free(tdev, &tfstart, &tfsize, &tgpt) != 0) {
                    snprintf(msg, sizeof msg, "No unpartitioned space on /dev/%s - use Erase mode", tdev);
                    scr = SCR_MSG; return;
                }
                /* partition number = next free index (max 4 on MBR) */
                char cmd[128], line[256]; int n = 0;
                snprintf(cmd, sizeof cmd, "sfdisk -d /dev/%s 2>/dev/null", tdev);
                FILE *f = popen(cmd, "r");
                while (f && fgets(line, sizeof line, f)) {
                    if (strstr(line, "start=")) n++;
                }
                if (f) pclose(f);
                if (!tgpt && n >= 4) {
                    snprintf(msg, sizeof msg, "MBR full on /dev/%s - use Erase mode", tdev);
                    scr = SCR_MSG; return;
                }
                mode_pnum = n + 1;
            }
            scr = SCR_CONFIRM;
        }
    } else if (scr == SCR_CONFIRM) {
        if (id == 0) { scr = SCR_DISK; }
        else if (id == 99) { start_install(); }
    } else if (scr == SCR_MSG) {
        scr = SCR_DISK;
    } else if (scr == SCR_DONE) {
        if (result_ok && id == 101) { step_run("sync"); system("reboot -f"); }
        else if (id == 102) { execl("/bin/busybox", "sh", NULL); exit(0); }
        else scr = SCR_MODE;
    }
    redraw();
}

/* ------------------------------------------------------------- input */
/*
 *  Input is the part that kept failing on real machines, so it is written
 *  defensively:
 *
 *   - every /dev/input/event* is opened and classified through EVIOCGBIT, so
 *     we do not care which node is the keyboard, which is the mouse and which
 *     is the touch panel (order differs per machine);
 *   - BTN_LEFT *and* BTN_TOUCH / BTN_TOOL_FINGER / ABS_MT_TRACKING_ID are all
 *     accepted as "press" - this is what makes touch panels work at all;
 *   - absolute ranges come from EVIOCGABS instead of a hard-coded 1024x768,
 *     and ABS_MT_POSITION_X/Y is handled as well as plain ABS_X/Y;
 *   - key-up is no longer swallowed: a press is latched and the release
 *     activates whatever button is under the pointer, with a 700 ms fallback
 *     for panels that never report a release;
 *   - stdin is read in raw mode as a second channel, so the console keyboard
 *     still works if the evdev nodes are missing or unreadable;
 *   - devices are re-scanned every 3 s to survive hotplug ordering.
 */
#define BITS(x)        (((x) + 7) / 8)
#define MAXDEV         12

typedef struct {
    int  fd;
    char path[48];
    int  is_abs;
    int  xcode, ycode;
    int  xmin, xmax, ymin, ymax;
    int  has_button;
    int  rx, ry;                 /* last raw absolute value */
} Dev;

static Dev  dv[MAXDEV];
static int  ndev = 0;
static int  press_btn = -1;      /* button index held down */
static long press_ms = 0;
static int  stdin_raw = 0;
static int  ev_kbd = 0;          /* an evdev keyboard exists */
static int  dirty = 1;

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}
static int bit_is_set(const unsigned char *bits, int n) {
    return (bits[n >> 3] >> (n & 7)) & 1;
}
static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static int dev_index(const char *path) {
    for (int i = 0; i < ndev; i++) if (!strcmp(dv[i].path, path)) return i;
    return -1;
}

static void add_dev(const char *path) {
    if (ndev >= MAXDEV || dev_index(path) >= 0) return;
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) return;

    unsigned char evbit[BITS(EV_CNT)]  = {0};
    unsigned char keybit[BITS(KEY_CNT)] = {0};
    unsigned char absbit[BITS(ABS_CNT)] = {0};
    if (ioctl(fd, EVIOCGBIT(0, sizeof evbit), evbit) < 0) { close(fd); return; }
    ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keybit), keybit);
    ioctl(fd, EVIOCGBIT(EV_ABS, sizeof absbit), absbit);

    Dev d;
    memset(&d, 0, sizeof d);
    d.fd = fd;
    strncpy(d.path, path, sizeof d.path - 1);
    d.xcode = d.ycode = -1;

    if (bit_is_set(absbit, ABS_MT_POSITION_X) && bit_is_set(absbit, ABS_MT_POSITION_Y)) {
        d.is_abs = 1; d.xcode = ABS_MT_POSITION_X; d.ycode = ABS_MT_POSITION_Y;
    } else if (bit_is_set(absbit, ABS_X) && bit_is_set(absbit, ABS_Y)) {
        d.is_abs = 1; d.xcode = ABS_X; d.ycode = ABS_Y;
    }
    d.has_button = bit_is_set(keybit, BTN_LEFT) || bit_is_set(keybit, BTN_TOUCH) ||
                   bit_is_set(keybit, BTN_TOOL_FINGER) || bit_is_set(keybit, BTN_STYLUS);
    if (bit_is_set(keybit, KEY_ENTER) || bit_is_set(keybit, KEY_A) ||
        bit_is_set(keybit, KEY_Q)) ev_kbd = 1;

    d.xmin = 0; d.xmax = 1023; d.ymin = 0; d.ymax = 767;
    if (d.is_abs) {
        struct input_absinfo ai;
        if (ioctl(fd, EVIOCGABS(d.xcode), &ai) == 0 && ai.maximum > ai.minimum) {
            d.xmin = ai.minimum; d.xmax = ai.maximum;
        }
        if (ioctl(fd, EVIOCGABS(d.ycode), &ai) == 0 && ai.maximum > ai.minimum) {
            d.ymin = ai.minimum; d.ymax = ai.maximum;
        }
    }
    d.rx = d.xmin + (d.xmax - d.xmin) / 2;
    d.ry = d.ymin + (d.ymax - d.ymin) / 2;
    dv[ndev++] = d;
}

static void open_inputs(void) {
    DIR *d = opendir("/dev/input");
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "event", 5)) continue;
        char p[64];
        snprintf(p, sizeof p, "/dev/input/%s", e->d_name);
        add_dev(p);
    }
    closedir(d);
}

/* absolute -> screen, using the device's own reported range */
static void set_abs(Dev *d, int rx, int ry) {
    int xr = d->xmax - d->xmin, yr = d->ymax - d->ymin;
    if (xr <= 0) xr = 1;
    if (yr <= 0) yr = 1;
    int sx = (rx - d->xmin) * (W - 1) / xr;
    int sy = (ry - d->ymin) * (H - 1) / yr;
    sx = clampi(sx, 0, W - 1);
    sy = clampi(sy, 0, H - 1);
    if (sx != mx || sy != my) { mx = sx; my = sy; dirty = 1; }
}

static int hit_test(void) {
    for (int b = 0; b < nbtn; b++) {
        Btn *t = &btns[b];
        if (mx >= t->x - 8 && mx <= t->x + t->w + 8 &&
            my >= t->y - 8 && my <= t->y + t->h + 8) return b;
    }
    return -1;
}

static void pointer_down(void) {
    if (mx < 0 || my < 0) {          /* first touch before any move event */
        mx = W / 2; my = H / 2; dirty = 1;
    }
    int b = hit_test();
    press_btn = b;
    press_ms = now_ms();
    if (b >= 0) { sel = b; dirty = 1; }
}
static void pointer_up(void) {
    if (press_btn >= 0) {
        int b = hit_test();          /* only fire if still over the same box */
        int id = btns[press_btn].id;
        int idx = press_btn;
        press_btn = -1;
        if (b == idx) activate(id);
        else dirty = 1;
    }
}

static void key_event(int code, int value) {
    if (value == 2) return;          /* autorepeat: ignore, we move by press */
    if (value == 0) return;          /* key release */
    FILE *t = fopen("/dev/ttyS0", "w");
    if (t) { fprintf(t, "[gui] key %d\n", code); fclose(t); }

    if (nbtn == 0) return;
    if (sel < 0 || sel >= nbtn) sel = 0;

    switch (code) {
    case KEY_UP: case KEY_LEFT:
        sel = (sel + nbtn - 1) % nbtn; dirty = 1; break;
    case KEY_DOWN: case KEY_RIGHT: case KEY_TAB:
        sel = (sel + 1) % nbtn; dirty = 1; break;
    case KEY_ENTER: case KEY_KPENTER: case KEY_SPACE:
        activate(btns[sel].id); break;
    case KEY_ESC:
        if (scr != SCR_MODE) activate(0);
        break;
    default:
        if (code >= KEY_1 && code <= KEY_9) {
            int n = code - KEY_1;
            if (n < nbtn) { sel = n; dirty = 1; activate(btns[n].id); }
        }
        break;
    }
}

static void handle_event(Dev *d, struct input_event *ev) {
    if (ev->type == EV_KEY) {
        if (ev->code == BTN_LEFT || ev->code == BTN_TOUCH ||
            ev->code == BTN_TOOL_FINGER || ev->code == BTN_STYLUS) {
            if (ev->value == 1) pointer_down();
            else if (ev->value == 0) pointer_up();
            return;
        }
        key_event(ev->code, ev->value);
    } else if (ev->type == EV_ABS) {
        if (ev->code == d->xcode)      { d->rx = ev->value; set_abs(d, d->rx, d->ry); }
        else if (ev->code == d->ycode) { d->ry = ev->value; set_abs(d, d->rx, d->ry); }
        else if (ev->code == ABS_MT_TRACKING_ID) {
            /* protocol B panels: a new finger id means touch, -1 means lift */
            if (ev->value >= 0) { if (!d->has_button) pointer_down(); }
            else                { if (!d->has_button) pointer_up(); }
        }
    } else if (ev->type == EV_REL) {
        if (mx < 0 || my < 0) { mx = W / 2; my = H / 2; }
        if (ev->code == REL_X) { mx = clampi(mx + ev->value * 2, 0, W - 1); dirty = 1; }
        if (ev->code == REL_Y) { my = clampi(my + ev->value * 2, 0, H - 1); dirty = 1; }
    }
}

/* ---- stdin fallback: console / serial keyboard in raw mode ------------- */
static void stdin_setup(void) {
    if (!isatty(0)) return;
    struct termios t;
    if (tcgetattr(0, &t) != 0) return;
    t.c_lflag &= ~(ICANON | ECHO | ISIG);
    t.c_cc[VMIN] = 0; t.c_cc[VTIME] = 0;
    if (tcsetattr(0, TCSANOW, &t) != 0) return;
    fcntl(0, F_SETFL, fcntl(0, F_GETFL) | O_NONBLOCK);
    stdin_raw = 1;
}

static void stdin_bytes(void) {
    static int esc = 0;
    unsigned char c;
    while (read(0, &c, 1) == 1) {
        if (esc == 1) { esc = (c == '[' || c == 'O') ? 2 : 0; continue; }
        if (esc == 2) {
            esc = 0;
            if (c == 'A') key_event(KEY_UP, 1);
            else if (c == 'B') key_event(KEY_DOWN, 1);
            else if (c == 'C') key_event(KEY_RIGHT, 1);
            else if (c == 'D') key_event(KEY_LEFT, 1);
            continue;
        }
        if (c == 27) { esc = 1; continue; }
        if (c == '\r' || c == '\n') key_event(KEY_ENTER, 1);
        else if (c == ' ') key_event(KEY_SPACE, 1);
        else if (c == '\t') key_event(KEY_TAB, 1);
        else if (c >= '1' && c <= '9') key_event(KEY_1 + (c - '1'), 1);
        else if (c == 'q' || c == 'Q') key_event(KEY_ESC, 1);
    }
}

static void pump_events(void) {
    fd_set r;
    FD_ZERO(&r);
    int mfd = 0;
    for (int i = 0; i < ndev; i++) { FD_SET(dv[i].fd, &r); if (dv[i].fd > mfd) mfd = dv[i].fd; }
    if (stdin_raw && !ev_kbd) { FD_SET(0, &r); }
    struct timeval tv = { 0, 40000 };      /* 40 ms tick */
    if (select(mfd + 1, &r, NULL, NULL, &tv) > 0) {
        for (int i = 0; i < ndev; i++) {
            if (!FD_ISSET(dv[i].fd, &r)) continue;
            struct input_event ev;
            while (read(dv[i].fd, &ev, sizeof ev) == sizeof ev)
                handle_event(&dv[i], &ev);
        }
        if (stdin_raw && !ev_kbd && FD_ISSET(0, &r)) stdin_bytes();
    }
    /* panels that report a touch but never a release: commit after 700 ms */
    if (press_btn >= 0 && now_ms() - press_ms > 700) {
        int id = btns[press_btn].id;
        press_btn = -1;
        activate(id);
    }
}

/* --------------------------------------------------------------- main */

/* Ask fbdev for the biggest mode the panel advertises; a 1080p/4K display
 * must not show a small 1024x768 stamp in the corner.  Silently keeps the
 * current mode if the driver refuses the switch. */
static void set_best_mode(void) {
    FILE *f = fopen("/sys/class/graphics/fb0/modes", "r");
    if (!f) return;
    char line[80];
    int bw = 0, bh = 0;
    while (fgets(line, sizeof line, f)) {
        char *c = strchr(line, ':');
        int w = 0, h = 0;
        if (c && sscanf(c + 1, "%dx%d", &w, &h) == 2) {
            if (w < 800 || w > 4096 || h < 600 || h > 2400) continue;
            if ((long)w * h > (long)bw * bh) { bw = w; bh = h; }
        }
    }
    fclose(f);
    if (bw == 0) return;

    struct fb_var_screeninfo v;
    if (ioctl(fbfd, FBIOGET_VSCREENINFO, &v)) return;
    if (bw <= v.xres && bh <= v.yres) return;
    unsigned int ox = v.xres, oy = v.yres;
    v.xres = bw; v.yres = bh;
    v.xres_virtual = bw; v.yres_virtual = bh;
    v.activate = FB_ACTIVATE_ALL;
    if (ioctl(fbfd, FBIOPUT_VSCREENINFO, &v)) {
        v.xres = ox; v.yres = oy;
        v.xres_virtual = ox; v.yres_virtual = oy;
        ioctl(fbfd, FBIOPUT_VSCREENINFO, &v);
        fprintf(stderr, "fb: keeping %ux%u\n", ox, oy);
    }
}
int main(void) {
    fbfd = open("/dev/fb0", O_RDWR);
    if (fbfd < 0) { perror("fb0"); return 1; }
    set_best_mode();
    struct fb_var_screeninfo v;
    struct fb_fix_screeninfo f;
    if (ioctl(fbfd, FBIOGET_VSCREENINFO, &v) || ioctl(fbfd, FBIOGET_FSCREENINFO, &f)) { perror("ioctl"); return 1; }
    W = v.xres; H = v.yres; BPP = v.bits_per_pixel; ROWL = f.line_length;
    if (BPP != 32 && BPP != 24 && BPP != 16) return 1;
    if (mx < 0) { mx = W / 2; my = H / 2; }
    FS = H / 360;                 /* 800p -> 2, 1080p -> 3, 1440p -> 4 */
    if (FS < 1) FS = 1;
    if (FS > 4) FS = 4;
    size_t len = (size_t)f.smem_len;
    fb = mmap(0, len, PROT_READ | PROT_WRITE, MAP_SHARED, fbfd, 0);
    if (fb == MAP_FAILED) { perror("mmap"); return 1; }

    scan_disks();
    open_inputs();
    stdin_setup();
    {   /* leave a breadcrumb for debugging from the serial console */
        FILE *t = fopen("/dev/ttyS0", "w");
        if (t) { fprintf(t, "[gui] devices=%d stdin_raw=%d fb=%dx%dx%d\n", ndev, stdin_raw, W, H, BPP); fclose(t); }
    }
    scr = SCR_MODE;
    redraw();
    dirty = 0;

    long last_scan = now_ms();
    for (;;) {
        pump_events();
        if (now_ms() - last_scan > 3000) { last_scan = now_ms(); open_inputs(); }
        if (dirty) { dirty = 0; redraw(); }
    }
    return 0;
}
