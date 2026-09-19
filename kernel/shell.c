/* NovaOS - kernel shell */
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

static char line[128];
static u32  line_len = 0;

static void prompt(void) {
    vga_write("novaos> ", 0x0B);
}

static void cmd_help(void) {
    vga_write(
        "commands: help clear meminfo tasks gui about reboot\n"
        "          ls  cat <file>  run <name>  echo <text>\n",
        0x0F);
}

static void cmd_meminfo(void) {
    char buf[96];
    ksprintf(buf,
        "phys free: %u KB of %u KB | heap used: %u B | cr3: 0x%x\n",
        mm_free_frames() * 4, (MEM_END - 0x100000) / 1024,
        kheap_used(), paging_get_cr3());
    vga_write(buf, 0x0F);
}

static void cmd_tasks(void) {
    char buf[64];
    ksprintf(buf, "tasks: %u total | current pid: %u\n",
             sched_task_count(), sched_current_pid());
    vga_write(buf, 0x0F);
}

static void cmd_echo(char *rest) {
    vga_write(rest, 0x0F);
    vga_write("\n", 0x0F);
}

static void cmd_about(void) {
    vga_write(
        "NovaOS v0.5 - self-made operating system\n"
        "layers: bootloader, GDT, IDT, PIC, PIT, keyboard, mouse, VGA,\n"
        "        paging (U/S split), phys allocator, heap, scheduler,\n"
        "        syscalls, ring3 userland, FAT12 reader, GUI desktop\n",
        0x0F);
}

static void cmd_ls(void) {
    char names[FAT_MAX_FILES][FAT_NAME_LEN];
    u32  sizes[FAT_MAX_FILES];
    u32  count = 0;
    char buf[64];
    if (!fat_list(names, sizes, FAT_MAX_FILES, &count)) {
        vga_write("ls: no FAT disk mounted\n", 0x0C);
        return;
    }
    if (count == 0) {
        vga_write("(empty)\n", 0x0F);
        return;
    }
    for (u32 i = 0; i < count; i++) {
        ksprintf(buf, "%-14s %8u B\n", names[i], sizes[i]);
        vga_write(buf, 0x0F);
    }
}

static void cmd_cat(const char *name) {
    u8 *buf = (u8*)kmalloc(4096);
    u32 size = 0;
    if (!buf) return;
    if (!fat_read(name, buf, 4096, &size)) {
        vga_write("cat: file not found: ", 0x0C);
        vga_write(name, 0x0C);
        vga_write("\n", 0x0C);
        kfree(buf);
        return;
    }
    buf[size] = '\0';
    vga_write((const char*)buf, 0x0F);
    if (size == 0 || buf[size - 1] != '\n') vga_write("\n", 0x0F);
    kfree(buf);
}

static void cmd_run(const char *name) {
    char fname[FAT_NAME_LEN];
    strncpy(fname, name, FAT_NAME_LEN - 1);
    fname[FAT_NAME_LEN - 1] = '\0';
    if (strchr(fname, '.') == NULL) strcat(fname, ".elf");

    u8 *img = (u8*)kmalloc(16384);
    u32 size = 0;
    char buf[64];
    if (!img) { vga_write("run: no heap\n", 0x0C); return; }
    if (!fat_read(fname, img, 16384, &size)) {
        vga_write("run: cannot load ", 0x0C);
        vga_write(fname, 0x0C);
        vga_write("\n", 0x0C);
        kfree(img);
        return;
    }
    u32 pid = user_spawn(fname, img, size);
    if (pid) {
        ksprintf(buf, "run: %s started as pid %u\n", fname, pid);
        vga_write(buf, 0x0A);
    } else {
        vga_write("run: bad ELF image\n", 0x0C);
    }
    kfree(img);
}

void shell_run(void) {
    prompt();
    for (;;) {
        int c = keyboard_getc();
        if (!c) continue;
        if (c == '\n') {
            vga_write("\n", 0x0F);
            line[line_len] = '\0';
            if (line_len > 0) {
                if      (strcmp(line, "help")    == 0) cmd_help();
                else if (strcmp(line, "clear")   == 0) vga_clear(0x0F);
                else if (strcmp(line, "meminfo") == 0) cmd_meminfo();
                else if (strcmp(line, "tasks")   == 0) cmd_tasks();
                else if (strcmp(line, "gui")     == 0) { gui_enter(); vga_clear(0x0F); }
                else if (strcmp(line, "about")   == 0) cmd_about();
                else if (strcmp(line, "ls")      == 0) cmd_ls();
                else if (strncmp(line, "cat ", 4) == 0) cmd_cat(line + 4);
                else if (strncmp(line, "run ", 4) == 0) cmd_run(line + 4);
                else if (strncmp(line, "echo ", 5) == 0) cmd_echo(line + 5);
                else if (strcmp(line, "reboot")  == 0) {
                    vga_write("rebooting...\n", 0x0C);
                    /* fast keyboard reset */
                    outb(0x64, 0xFE);
                    for (;;) hlt();
                }
                else {
                    vga_write("unknown command: ", 0x0C);
                    vga_write(line, 0x0C);
                    vga_write("\n", 0x0C);
                }
            }
            line_len = 0;
            prompt();
        } else if (c == '\b') {
            if (line_len > 0) { line_len--; vga_putc('\b', 0x0F); vga_putc(' ', 0x0F); vga_putc('\b', 0x0F); }
        } else if (c >= ' ' && line_len < sizeof(line) - 1) {
            line[line_len++] = (char)c;
            vga_putc((char)c, 0x0F);
        }
    }
}
