/* NovaOS - kernel shell
 *
 * A small but complete interactive shell.  Design notes:
 *
 *  - Input is line based.  A ring buffer of previous commands plus tab
 *    completion is kept in the shell so the keyboard driver stays a dumb
 *    character source.
 *  - Every command parser splits the line into argv[] once, so a command can
 *    inspect its own argument count instead of re-walking the string.  That is
 *    what fixed "cat" with no argument printing a bare "cat: file not found"
 *    and every "foo   bar" (double space) form being a syntax error.
 *  - All error paths print to the console *and* are reported with a distinct
 *    colour, so a failed command is obvious at a glance.
 */
#include "shell.h"
#include "vga.h"
#include "keyboard.h"
#include "string.h"
#include "printf.h"
#include "serial.h"
#include "io.h"
#include "mm.h"
#include "kheap.h"
#include "sched.h"
#include "timer.h"
#include "gui.h"
#include "paging.h"
#include "fat.h"
#include "user.h"

#define LINE_MAX   128
#define ARGV_MAX   16
#define HIST_MAX   8

static char line[LINE_MAX];
static u32  line_len = 0;

/* command history: circular, scrolls with the up/down arrows */
static char hist[HIST_MAX][LINE_MAX];
static u32  hist_count = 0;          /* total commands ever entered */
static int  hist_pos   = -1;         /* -1 = editing a fresh line */

/* ------------------------------------------------------------------ */
/* small output helpers                                               */
/* ------------------------------------------------------------------ */

#define C_OK    0x0A
#define C_ERR   0x0C
#define C_DIM   0x07

/* Console output goes to the VGA text screen *and* the serial port, so the
 * shell can be driven and captured over a serial console (QEMU -serial) as
 * well as watched on screen.  Keeping the two in one place is what makes
 * the boot log and the interactive session appear in the same capture. */
static void con_putc(char c) {
    vga_putc(c, 0x0F);
    serial_putc(c);
}
static void con_write(const char *s, u8 attr) {
    vga_write(s, attr);
    for (u32 i = 0; s[i]; i++) serial_putc(s[i]);
}

static void puts(const char *s)  { con_write(s, 0x0F); }
static void putsn(const char *s) { con_write(s, 0x0F); con_write("\n", 0x0F); }
static void err(const char *s)   { con_write(s, C_ERR); }
static void ok(const char *s)    { con_write(s, C_OK); }

static void prompt(void) {
    con_write("novaos> ", 0x0B);
}

/* redraw the current editing line (used after a history recall) */
static void redraw_line(void) {
    con_write("\r", 0x0F);
    /* wipe the 8-char prompt plus any leftovers from a longer old line */
    for (int i = 0; i < 80; i++) con_putc(' ');
    con_write("\r", 0x0F);
    prompt();
    for (u32 i = 0; i < line_len; i++) con_putc(line[i]);
}

static void remember(const char *cmd) {
    if (cmd[0] == '\0') return;
    /* skip immediate duplicates, like a real shell */
    if (hist_count > 0) {
        u32 last = (hist_count - 1) % HIST_MAX;
        if (strcmp(hist[last], cmd) == 0) return;
    }
    strncpy(hist[hist_count % HIST_MAX], cmd, LINE_MAX - 1);
    hist[hist_count % HIST_MAX][LINE_MAX - 1] = '\0';
    hist_count++;
    hist_pos = -1;
}

/* recall the n-th most recent command (0 = most recent) */
static const char *hist_get(u32 back) {
    if (back >= HIST_MAX || back >= hist_count) return NULL;
    return hist[(hist_count - 1 - back) % HIST_MAX];
}

/* ------------------------------------------------------------------ */
/* argument splitting                                                 */
/* ------------------------------------------------------------------ */

/* Split a line in place on runs of spaces (so "cat   a.txt" works too).
 * Returns the number of tokens; argv[] points into line[]. */
static int split(char *s, char *argv[], int max) {
    int n = 0;
    while (*s) {
        while (*s == ' ' || *s == '\t') s++;
        if (!*s) break;
        if (n >= max - 1) break;              /* keep the tail as one token */
        argv[n++] = s;
        while (*s && *s != ' ' && *s != '\t') s++;
        if (*s) *s++ = '\0';
    }
    argv[n] = NULL;
    return n;
}

static u32 slen(const char *s) { u32 n = 0; while (s[n]) n++; return n; }

/* ------------------------------------------------------------------ */
/* commands                                                           */
/* ------------------------------------------------------------------ */

static void cmd_help(void) {
    con_write(
        "NovaOS shell - built-in commands\n"
        "  help              this text\n"
        "  clear             clear the screen\n"
        "  meminfo           physical / heap memory usage\n"
        "  tasks             running tasks\n"
        "  ps                task table with pid / name / state\n"
        "  ls                list the FAT12 root directory\n"
        "  cat <file>        print a file from the data disk\n"
        "  hexdump <file>    dump a file as hex + ASCII\n"
        "  stat <file>       size / first cluster / attribute\n"
        "  write <f> <text>  create or replace a file with <text>\n"
        "  append <f> <text> add <text> to the end of a file\n"
        "  rm <file>         delete a file\n"
        "  df                free space on the data disk\n"
        "  run <prog>        load a program from disk and run it in ring 3\n"
        "  echo <text>       print the arguments back\n"
        "  uptime            ticks since boot\n"
        "  gui               enter the graphical desktop\n"
        "  about             version / feature summary\n"
        "  reboot            reset the machine\n"
        "\n"
        "editing: <backspace> delete, <up>/<down> history, <tab> complete\n",
        0x0F);
}

static void cmd_clear(void) {
    vga_clear(0x0F);
}

static void cmd_meminfo(void) {
    char buf[128];
    u32 total = (MEM_END - 0x100000) / 1024;
    u32 freek = mm_free_frames() * 4;
    u32 used  = total > freek ? total - freek : 0;
    ksprintf(buf, "physical: %u KB total, %u KB free, %u KB used\n",
             total, freek, used);
    puts(buf);
    ksprintf(buf, "heap    : 0x300000-0x400000, %u B used\n", kheap_used());
    puts(buf);
    ksprintf(buf, "paging  : current cr3 = 0x%x\n", paging_get_cr3());
    puts(buf);
}

static void cmd_tasks(void) {
    char buf[80];
    ksprintf(buf, "tasks: %u total | current pid: %u\n",
             sched_task_count(), sched_current_pid());
    puts(buf);
}

static void cmd_ps(void) {
    char buf[80];
    u32 n = sched_task_count();
    puts("  slot pid  state name\n");
    for (u32 i = 0; i < n; i++) {
        ksprintf(buf, "  %4u %4u  %5s %s\n",
                 i, sched_task_pid(i),
                 sched_task_state(i) ? "ready" : "dead",
                 sched_task_name(i));
        puts(buf);
    }
}

static void cmd_uptime(void) {
    char buf[64];
    u32 t = timer_ticks();
    ksprintf(buf, "up %u ticks (%u.%02u s at 100 Hz)\n",
             t, t / 100, t % 100);
    puts(buf);
}

static void cmd_echo(char *rest) {
    putsn(rest);
}

static void cmd_about(void) {
    con_write(
        "NovaOS v1.0 - a small operating system written from scratch\n"
        "  boot    : 512B stage1 -> stage2 loader -> protected mode -> kernel\n"
        "  drivers : VGA text + mode-13h graphics, PS/2 keyboard & mouse,\n"
        "            8259 PIC, PIT, ATA PIO\n"
        "  memory  : physical frame allocator, kernel heap, per-process page\n"
        "            tables with copy-on-write fork\n"
        "  sched   : preemptive round robin, private ring0 stack per task\n"
        "  syscall : int 0x80 - write getpid exit fork open read close sleep brk\n"
        "  fs      : FAT12 reader (8.3 names, cluster chain, spc aware)\n"
        "  userland: static ELF32 loaded at 0x200000, runs in ring 3\n",
        0x0F);
}

static void cmd_ls(void) {
    static char names[FAT_MAX_FILES][FAT_NAME_LEN];
    static u32  sizes[FAT_MAX_FILES];
    u32  count = 0;
    char buf[64];
    if (!fat_list(names, sizes, FAT_MAX_FILES, &count)) {
        err("ls: no FAT disk mounted\n");
        return;
    }
    if (count == 0) {
        puts("(empty)\n");
        return;
    }
    u32 total = 0;
    for (u32 i = 0; i < count; i++) {
        ksprintf(buf, "  %-14s %8u B\n", names[i], sizes[i]);
        puts(buf);
        total += sizes[i];
    }
    ksprintf(buf, "  %u file(s), %u B\n", count, total);
    puts(buf);
}

static void cmd_cat(const char *name) {
    u8 *buf = (u8*)kmalloc(4096);
    u32 size = 0;
    if (!buf) { err("cat: out of heap\n"); return; }
    if (!fat_read(name, buf, 4096, &size)) {
        err("cat: no such file: ");
        err(name);
        con_write("\n", C_ERR);
        kfree(buf);
        return;
    }
    if (size == 0) {
        puts("(empty file)\n");
        kfree(buf);
        return;
    }
    for (u32 i = 0; i < size; i++) con_putc((char)buf[i]);
    if (buf[size - 1] != '\n') con_write("\n", 0x0F);
    kfree(buf);
}

/* hex dump: 16 bytes per row, offset + hex + printable ASCII */
static void cmd_hexdump(const char *name) {
    static const char hexd[] = "0123456789abcdef";
    u8 *buf = (u8*)kmalloc(4096);
    u32 size = 0;
    char row[80];
    if (!buf) { err("hexdump: out of heap\n"); return; }
    if (!fat_read(name, buf, 4096, &size)) {
        err("hexdump: no such file: ");
        err(name);
        con_write("\n", C_ERR);
        kfree(buf);
        return;
    }
    for (u32 base = 0; base < size; base += 16) {
        u32 p = 0;
        p += (u32)ksprintf(row + p, "%08x  ", base);
        for (u32 i = 0; i < 16; i++) {
            if (base + i < size) {
                row[p++] = hexd[buf[base + i] >> 4];
                row[p++] = hexd[buf[base + i] & 0xF];
            } else {
                row[p++] = ' '; row[p++] = ' ';
            }
            row[p++] = ' ';
        }
        row[p++] = '|';
        for (u32 i = 0; i < 16 && base + i < size; i++) {
            u8 c = buf[base + i];
            row[p++] = (c >= 32 && c < 127) ? (char)c : '.';
        }
        row[p++] = '|';
        row[p++] = '\n';
        row[p]   = '\0';
        puts(row);
    }
    ksprintf(row, "%u byte(s)\n", size);
    puts(row);
    kfree(buf);
}

static void cmd_run(const char *name) {
    char fname[FAT_NAME_LEN];
    strncpy(fname, name, FAT_NAME_LEN - 1);
    fname[FAT_NAME_LEN - 1] = '\0';
    if (strchr(fname, '.') == NULL) strcat(fname, ".elf");

    u8 *img = (u8*)kmalloc(16384);
    u32 size = 0;
    char buf[80];
    if (!img) { err("run: out of heap\n"); return; }
    if (!fat_read(fname, img, 16384, &size)) {
        err("run: cannot load ");
        err(fname);
        con_write("\n", C_ERR);
        kfree(img);
        return;
    }
    u32 pid = user_spawn(fname, img, size);
    if (pid) {
        ksprintf(buf, "run: %s started as pid %u\n", fname, pid);
        ok(buf);
    } else {
        err("run: not a valid ELF image\n");
    }
    kfree(img);
}

static void cmd_stat(const char *name) {
    u32 size = 0; u16 cluster = 0; u8 attr = 0;
    char buf[96];
    if (!fat_stat(name, &size, &cluster, &attr)) {
        err("stat: no such file: ");
        err(name);
        con_write("\n", C_ERR);
        return;
    }
    ksprintf(buf, "name    : %s\n", name);
    puts(buf);
    ksprintf(buf, "size    : %u bytes\n", size);
    puts(buf);
    ksprintf(buf, "cluster : %u\n", cluster);
    puts(buf);
    ksprintf(buf, "attr    : 0x%x%s\n", attr,
             (attr & 0x10) ? " (directory)" : "");
    puts(buf);
}

static void cmd_df(void) {
    char buf[80];
    if (!fat_mounted()) { err("df: no FAT disk mounted\n"); return; }
    ksprintf(buf, "data disk: %u bytes free\n", fat_free_bytes());
    puts(buf);
}

/* write/append: the text is everything after "file ".  Writing a file is
 * what turns the filesystem from a reader into something the machine can
 * actually keep state in. */
static void cmd_write(char *args, int append_mode) {
    /* args = "<name> <text...>" */
    char *name = args;
    while (*name == ' ') name++;
    char *sp = name;
    while (*sp && *sp != ' ') sp++;
    if (!*sp) {
        err(append_mode ? "append: usage: append <file> <text>\n"
                        : "write: usage: write <file> <text>\n");
        return;
    }
    *sp = '\0';
    char *text = sp + 1;
    while (*text == ' ') text++;
    u32 tlen = 0; while (text[tlen]) tlen++;

    static u8 buf[4096];
    u32 size = 0;

    if (append_mode) {
        /* read the tail, then rewrite the file with the extra bytes */
        if (fat_stat(name, &size, NULL, NULL)) {
            if (size > 4096 - tlen) { err("append: file too large\n"); return; }
            if (!fat_read(name, buf, 4096 - tlen, &size)) {
                err("append: read failed\n");
                return;
            }
        } else {
            size = 0;
        }
    } else {
        size = 0;
    }
    for (u32 i = 0; i < tlen; i++) buf[size + i] = (u8)text[i];
    u32 total = size + tlen;

    if (!fat_write(name, buf, total)) {
        err("write: failed (disk full or directory full)\n");
        return;
    }
    char msg[96];
    ksprintf(msg, "%s: %s (%u bytes)\n",
             append_mode ? "appended" : "wrote", name, total);
    ok(msg);
}

static void cmd_rm(const char *name) {
    if (!fat_delete(name)) {
        err("rm: no such file: ");
        err(name);
        con_write("\n", C_ERR);
        return;
    }
    ok("removed: ");
    ok(name);
    con_write("\n", C_OK);
}

/* ------------------------------------------------------------------ */
/* tab completion                                                     */
/* ------------------------------------------------------------------ */

static const char *builtins[] = {
    "help", "clear", "meminfo", "tasks", "ps", "ls", "cat", "hexdump",
    "run", "echo", "uptime", "gui", "about", "reboot",
    "stat", "df", "write", "append", "rm", NULL
};

/* complete "cat rea<TAB>" -> "cat readme.txt" */
static void complete(void) {
    /* find the start of the token under the cursor */
    u32 start = line_len;
    while (start > 0 && line[start - 1] != ' ') start--;
    u32 tlen = line_len - start;
    if (tlen == 0) return;

    const char *match = NULL;
    int matches = 0;

    /* first token: complete against the built-in command names */
    if (start == 0) {
        for (int i = 0; builtins[i]; i++) {
            if (strncmp(builtins[i], line, tlen) == 0) {
                match = builtins[i];
                matches++;
            }
        }
    }

    /* otherwise (and for `cat`/`hexdump`/`run`) complete against the disk */
    if (start > 0 && fat_mounted()) {
        static char names[FAT_MAX_FILES][FAT_NAME_LEN];
        static u32  sizes[FAT_MAX_FILES];
        u32 count = 0;
        if (fat_list(names, sizes, FAT_MAX_FILES, &count)) {
            for (u32 i = 0; i < count; i++) {
                /* the on-disk name is uppercase; compare case-insensitively */
                u32 k = 0;
                int same = 1;
                while (k < tlen && names[i][k]) {
                    char a = names[i][k];
                    char b = line[start + k];
                    if (b >= 'a' && b <= 'z') b -= 32;
                    if (a != b) { same = 0; break; }
                    k++;
                }
                if (same && k == tlen) {
                    match = names[i];
                    matches++;
                }
            }
        }
    }

    if (matches != 1 || !match) {
        if (matches > 1) con_write("\n(multiple matches)\n", C_DIM);
        return;                                  /* 0 or >1: do nothing */
    }

    /* append the remainder of the match, lowercased for readability */
    for (u32 k = tlen; match[k] && line_len < LINE_MAX - 1; k++) {
        char c = match[k];
        if (c >= 'A' && c <= 'Z') c += 32;
        line[line_len++] = c;
        con_putc(c);
    }
}

/* ------------------------------------------------------------------ */
/* command dispatch                                                   */
/* ------------------------------------------------------------------ */

static void execute(char *cmdline) {
    char *argv[ARGV_MAX];
    int argc = split(cmdline, argv, ARGV_MAX);
    if (argc == 0) return;

    const char *c = argv[0];

    if      (strcmp(c, "help")    == 0) cmd_help();
    else if (strcmp(c, "clear")   == 0) cmd_clear();
    else if (strcmp(c, "meminfo") == 0) cmd_meminfo();
    else if (strcmp(c, "tasks")   == 0) cmd_tasks();
    else if (strcmp(c, "ps")      == 0) cmd_ps();
    else if (strcmp(c, "uptime")  == 0) cmd_uptime();
    else if (strcmp(c, "gui")     == 0) { gui_enter(); vga_clear(0x0F); }
    else if (strcmp(c, "about")   == 0) cmd_about();
    else if (strcmp(c, "ls")      == 0) cmd_ls();
    else if (strcmp(c, "reboot")  == 0) {
        err("rebooting...\n");
        outb(0x64, 0xFE);                   /* fast keyboard reset */
        for (;;) hlt();
    }
    else if (strcmp(c, "cat")     == 0) {
        if (argc < 2) err("cat: missing file operand (try `ls`)\n");
        else          cmd_cat(argv[1]);
    }
    else if (strcmp(c, "hexdump") == 0) {
        if (argc < 2) err("hexdump: missing file operand\n");
        else          cmd_hexdump(argv[1]);
    }
    else if (strcmp(c, "stat") == 0) {
        if (argc < 2) err("stat: missing file operand\n");
        else          cmd_stat(argv[1]);
    }
    else if (strcmp(c, "df") == 0) {
        cmd_df();
    }
    else if (strcmp(c, "write") == 0 || strcmp(c, "append") == 0) {
        if (argc < 3) {
            err(strcmp(c, "write") == 0
                ? "write: usage: write <file> <text>\n"
                : "append: usage: append <file> <text>\n");
        } else {
            /* split() turned every separator into a NUL, so the original
             * text is gone.  Re-join argv[1..] with single spaces: that
             * keeps "write f a b" working and is what a shell would store
             * for a quoted-free argument list anyway. */
            static char tail[LINE_MAX];
            u32 w = 0;
            for (int i = 1; i < argc && w < LINE_MAX - 2; i++) {
                if (i > 1 && w < LINE_MAX - 2) tail[w++] = ' ';
                for (u32 k = 0; argv[i][k] && w < LINE_MAX - 2; k++)
                    tail[w++] = argv[i][k];
            }
            tail[w] = '\0';
            cmd_write(tail, strcmp(c, "append") == 0);
        }
    }
    else if (strcmp(c, "rm") == 0) {
        if (argc < 2) err("rm: missing file operand\n");
        else          cmd_rm(argv[1]);
    }
    else if (strcmp(c, "run")     == 0) {
        if (argc < 2) err("run: missing program name\n");
        else          cmd_run(argv[1]);
    }
    else if (strcmp(c, "echo")    == 0) {
        static char tail[LINE_MAX];
        u32 w = 0;
        for (int i = 1; i < argc && w < LINE_MAX - 2; i++) {
            if (i > 1 && w < LINE_MAX - 2) tail[w++] = ' ';
            for (u32 k = 0; argv[i][k] && w < LINE_MAX - 2; k++)
                tail[w++] = argv[i][k];
        }
        tail[w] = '\0';
        cmd_echo(tail);
    }
    else {
        err("unknown command: ");
        err(c);
        err("  (try `help`)\n");
    }
}

/* ------------------------------------------------------------------ */
/* main loop                                                          */
/* ------------------------------------------------------------------ */

void shell_run(void) {
    prompt();
    for (;;) {
        int c = keyboard_getc();
        if (!c) c = serial_getc();       /* also accept a serial console */
        if (c < 0) continue;
        if (!c) continue;

        if (c == '\r') c = '\n';         /* serial terminals send CR */
        if (c == 0x7F) c = '\b';         /* serial backspace is DEL */

        if (c == '\n') {
            con_write("\n", 0x0F);
            line[line_len] = '\0';
            remember(line);
            execute(line);
            line_len = 0;
            hist_pos = -1;
            prompt();
        }
        else if (c == '\b') {
            if (line_len > 0) {
                line_len--;
                con_putc('\b');
                con_putc(' ');
                con_putc('\b');
            }
        }
        else if (c == '\t') {
            complete();
        }
        else if (c == 0x100 + 'A') {          /* up arrow: older command */
            u32 want = (hist_pos < 0) ? 0 : (u32)(hist_pos + 1);
            const char *h = hist_get(want);
            if (h) {
                hist_pos = (int)want;
                strncpy(line, h, LINE_MAX - 1);
                line[LINE_MAX - 1] = '\0';
                line_len = slen(line);
                redraw_line();
            }
        }
        else if (c == 0x100 + 'B') {          /* down arrow: newer command */
            if (hist_pos > 0) {
                hist_pos--;
                const char *h = hist_get((u32)hist_pos);
                if (h) {
                    strncpy(line, h, LINE_MAX - 1);
                    line[LINE_MAX - 1] = '\0';
                }
            } else {
                hist_pos = -1;
                line[0] = '\0';
            }
            line_len = slen(line);
            redraw_line();
        }
        else if (c >= ' ' && c < 0x100 && line_len < LINE_MAX - 1) {
            line[line_len++] = (char)c;
            con_putc((char)c);
        }
    }
}
