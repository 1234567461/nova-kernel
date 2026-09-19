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

static char line[128];
static u32  line_len = 0;

static void prompt(void) {
    vga_write("novaos> ", 0x0B);
}

static void cmd_help(void) {
    vga_write(
        "commands: help  clear  meminfo  tasks  gui  echo  about  reboot\n",
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
        "NovaOS v0.1 - self-made microkernel\n"
        "layers: bootloader, GDT, IDT, PIC, PIT, keyboard, VGA,\n"
        "        paging, physical allocator, kernel heap, scheduler,\n"
        "        syscalls, GUI desktop (mode 13h)\n",
        0x0F);
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
