/* ============================================================================
 *  Nova-Linux 0.9 (Aurora) - graphical install wizard
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
#include <dirent.h>
#include <errno.h>
#include <stdint.h>
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
        rect(b->x - 4, b->y - 4, b->w + 8, b->h + 8, on ? C_ACCENT : C_PANEL);
        rect(b->x, b->y, b->w, b->h, on ? 0x3A2A20 : C_PANEL);
        int tw = (int)strlen(b->label) * FONT_W * 2;
        text(b->x + (b->w - tw) / 2, b->y + (b->h - FONT_H * 2) / 2, b->label,
             on ? C_ACCENT : C_TEXT, 2);
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

static void header(void) {
    rect(0, 0, W, 64, C_PANEL);
    rect(0, 64, W, 3, C_ACCENT);
    text(24, 18, "Nova-Linux 0.9 (Aurora)", C_TEXT, 2);
    text(W - 200, 26, "Install Wizard", C_ACCENT, 1);
}

static void scr_mode(void) {
    btn_clear();
    header();
    center(H / 4, "Welcome to Nova-Linux", C_TEXT, 3);
    center(H / 4 + 44, "Choose what to do:", C_DIM, 2);
    int bw = 640, bx = (W - bw) / 2, by = H / 2 - 120;
    btn_add(bx, by,          bw, 72, "Erase entire disk & install", 1);
    btn_add(bx, by + 88,     bw, 72, "Install alongside existing OS (free space)", 2);
    btn_add(bx, by + 176,    bw, 72, "Live shell (no install)", 3);
    draw_btns(); flush();
}

static void scr_disk(void) {
    btn_clear();
    header();
    center(H / 4, mode ? "Install alongside - pick target disk" :
                        "Erase & install - pick target disk", C_TEXT, 2);
    if (!ndisk) {
        center(H / 2, "No disks found in this machine.", C_WARN, 2);
        btn_add((W - 320) / 2, H - 140, 320, 60, "Back", 0);
        draw_btns(); flush(); return;
    }
    int bw = 680, bx = (W - bw) / 2, by = H / 2 - 100;
    char line[96];
    for (int i = 0; i < ndisk && i < MAXBTN - 1; i++) {
        snprintf(line, sizeof line, "/dev/%s  -  %ld MB", disks[i].dev, disks[i].mb);
        btn_add(bx, by + i * 80, bw, 64, "", 10 + i);
        Btn *b = &btns[nbtn - 1];
        int on = (nbtn - 1 == sel);
        rect(b->x - 4, b->y - 4, b->w + 8, b->h + 8, on ? C_ACCENT : C_PANEL);
        rect(b->x, b->y, b->w, b->h, on ? 0x3A2A20 : C_PANEL);
        text(b->x + 24, b->y + (b->h - FONT_H) / 2, line, on ? C_ACCENT : C_TEXT, 1);
    }
    btn_add((W - 320) / 2, H - 130, 320, 60, "Back", 0);
    draw_btns(); flush();
}

static void scr_confirm(const char *dev, long fstart, long fsize) {
    btn_clear();
    header();
    center(H / 4, "Ready to install", C_TEXT, 3);
    char line[128];
    if (mode) {
        long mb = fsize * 512 / 1024 / 1024;
        snprintf(line, sizeof line, "Mode: alongside  |  /dev/%s%d  (start %ld, %ld MB)",
                 dev, mode_pnum, fstart, mb);
    } else {
        snprintf(line, sizeof line, "Mode: ERASE ALL DATA on /dev/%s", dev);
    }
    center(H / 2 - 60, line, mode ? C_TEXT : C_WARN, 2);
    center(H / 2 - 20, mode ? "Your existing system will NOT be touched."
                            : "Everything on this disk will be destroyed!",
           mode ? C_DIM : C_WARN, 2);
    center(H / 2 + 30, mode ? "After install: run 'update-grub' in your existing OS"
                            : "Nova-Linux will own the whole disk.", C_DIM, 1);
    int bw = 640, bx = (W - bw) / 2, by = H / 2 + 90;
    btn_add(bx, by, bw, 72, mode ? "Install now (safe)" : "ERASE & INSTALL NOW", 99);
    btn_add(bx, by + 88, bw, 60, "Cancel", 0);
    draw_btns(); flush();
}

static void scr_run(void) {
    btn_clear();
    header();
    center(100, "Installing Nova-Linux ...", C_TEXT, 3);
    rect((W - 700) / 2, 220, 700, 26, C_PANEL);
    rect((W - 700) / 2, 220, 700 * step_i / step_n, 26, C_ACCENT);
    char buf[1200];
    tail_log(buf, sizeof buf, 14);
    int y = 300;
    for (char *s = strtok(buf, "\n"); s; s = strtok(NULL, "\n")) {
        text(80, y, s, C_DIM, 1); y += 20;
        if (y > H - 60) break;
    }
    flush();
}

static void scr_done(int ok) {
    btn_clear();
    header();
    center(H / 3, ok ? "Installation complete!" : "Installation failed", ok ? C_OK : C_WARN, 3);
    center(H / 3 + 50, ok ? "Nova-Linux is on your disk." : "See log above / live shell for details.",
           C_DIM, 2);
    int bw = 560, bx = (W - bw) / 2, by = H / 2 + 60;
    if (ok) {
        btn_add(bx, by, bw, 72, "Reboot into Nova-Linux", 101);
        btn_add(bx, by + 88, bw, 60, "Exit to shell", 102);
    } else {
        btn_add(bx, by, bw, 60, "Exit to shell", 102);
    }
    draw_btns(); flush();
}

static void scr_msg(void) {
    btn_clear();
    header();
    center(H / 3, msg, C_WARN, 2);
    btn_add((W - 320) / 2, H / 2 + 40, 320, 60, "OK", 0);
    draw_btns(); flush();
}

/* wrappers for uniform screen table */
static void scr_confirm_w(void) { scr_confirm(tdev, tfstart, tfsize); }
static void scr_done_w(void) { scr_done(result_ok); }
static void (*screens[])(void) = { scr_mode, scr_disk, scr_confirm_w, scr_run, scr_done_w, scr_msg };
static void redraw(void) {
    rect(0, 0, W, H, C_BG);
    screens[scr]();
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
static int evfd[8], nev = 0;
static int relx = 0, rely = 0, btn_down = 0;

static void open_inputs(void) {
    DIR *d = opendir("/dev/input");
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && nev < 8) {
        if (strncmp(e->d_name, "event", 5)) continue;
        char p[64];
        snprintf(p, sizeof p, "/dev/input/%s", e->d_name);
        int fd = open(p, O_RDONLY | O_NONBLOCK);
        if (fd >= 0) evfd[nev++] = fd;
    }
    closedir(d);
}

static void pump_events(void) {
    fd_set r; FD_ZERO(&r);
    int mx_fd = 0;
    for (int i = 0; i < nev; i++) { FD_SET(evfd[i], &r); if (evfd[i] > mx_fd) mx_fd = evfd[i]; }
    struct timeval tv = { 0, 50000 };   /* 50ms -> blink/poll */
    if (select(mx_fd + 1, &r, NULL, NULL, &tv) <= 0) return;
    for (int i = 0; i < nev; i++) {
        if (!FD_ISSET(evfd[i], &r)) continue;
        struct input_event ev;
        while (read(evfd[i], &ev, sizeof ev) == sizeof ev) {
            if (ev.type == EV_KEY) {
                { FILE *d = fopen("/dev/ttyS0", "w"); if (d) { fprintf(d, "[gui] key %d val %d\n", ev.code, ev.value); fclose(d); } }
                if (ev.value == 0) continue;      /* ignore key-up */
                int code = ev.code;
                int id = -1;
                if (code == KEY_UP || code == KEY_LEFT)  { sel = (sel + nbtn - 1) % nbtn; redraw(); }
                else if (code == KEY_DOWN || code == KEY_RIGHT) { sel = (sel + 1) % nbtn; redraw(); }
                else if (code == KEY_ENTER || code == KEY_KPENTER || code == KEY_SPACE) activate(btns[sel].id);
                else if (code == KEY_ESC) { if (scr == SCR_DISK || scr == SCR_CONFIRM || scr == SCR_MSG) activate(0); }
                else if (code >= KEY_1 && code <= KEY_9) { id = code - KEY_1 + 1; if (scr == SCR_MODE) activate(id); }
                (void)id;
            } else if (ev.type == EV_REL) {
                if (ev.code == REL_X) mx += ev.value;
                if (ev.code == REL_Y) my += ev.value;
                if (mx < 0) mx = 0; if (my < 0) my = 0;
                if (mx > W - 1) mx = W - 1; if (my > H - 1) my = H - 1;
                redraw();
            } else if (ev.type == EV_ABS) {
                if (ev.code == ABS_X) { if (mx < 0) mx = ev.value * W / 1024; else mx = ev.value * W / 1024; }
                if (ev.code == ABS_Y) { if (my < 0) my = ev.value * H / 768; else my = ev.value * H / 768; }
                redraw();
            } else if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
                if (btn_down) { /* click release handled below */ }
            }
            if (ev.type == EV_KEY && ev.code == BTN_LEFT) {
                if (ev.value == 1) {
                    btn_down = 1;
                    /* hit test */
                    for (int b = 0; b < nbtn; b++) {
                        Btn *bt = &btns[b];
                        if (mx >= bt->x - 4 && mx <= bt->x + bt->w + 4 &&
                            my >= bt->y - 4 && my <= bt->y + bt->h + 4) {
                            sel = b; redraw();
                            break;
                        }
                    }
                } else if (ev.value == 0 && btn_down) {
                    btn_down = 0;
                    for (int b = 0; b < nbtn; b++) {
                        Btn *bt = &btns[b];
                        if (mx >= bt->x - 4 && mx <= bt->x + bt->w + 4 &&
                            my >= bt->y - 4 && my <= bt->y + bt->h + 4) {
                            activate(bt->id);
                            break;
                        }
                    }
                }
            }
        }
    }
}

/* --------------------------------------------------------------- main */
int main(void) {
    fbfd = open("/dev/fb0", O_RDWR);
    if (fbfd < 0) { perror("fb0"); return 1; }
    struct fb_var_screeninfo v;
    struct fb_fix_screeninfo f;
    if (ioctl(fbfd, FBIOGET_VSCREENINFO, &v) || ioctl(fbfd, FBIOGET_FSCREENINFO, &f)) { perror("ioctl"); return 1; }
    W = v.xres; H = v.yres; BPP = v.bits_per_pixel; ROWL = f.line_length;
    if (BPP != 32 && BPP != 24 && BPP != 16) return 1;
    size_t len = (size_t)f.smem_len;
    fb = mmap(0, len, PROT_READ | PROT_WRITE, MAP_SHARED, fbfd, 0);
    if (fb == MAP_FAILED) { perror("mmap"); return 1; }

    scan_disks();
    open_inputs();
    scr = SCR_MODE;
    redraw();
    for (;;) pump_events();
    return 0;
}
