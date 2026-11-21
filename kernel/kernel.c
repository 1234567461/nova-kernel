/* NovaOS - kernel entry point */
#include "common.h"
#include "io.h"
#include "serial.h"
#include "vga.h"
#include "gdt.h"
#include "idt.h"
#include "irq.h"
#include "timer.h"
#include "keyboard.h"
#include "mouse.h"
#include "mm.h"
#include "kheap.h"
#include "paging.h"
#include "sched.h"
#include "shell.h"
#include "user.h"
#include "ata.h"
#include "fat.h"
#include "bootlog.h"
#include "net.h"
#include "ne2k.h"
#include "update.h"
#include "ata.h"
#include "rtc.h"
#include "printf.h"
#include "string.h"
#include "printf.h"

/* demo kernel threads - each beats on the serial console a few times so
 * round-robin scheduling is observable even with no display attached */
static void demo_task_a(void) {
    u32 n = 0;
    while (timer_ticks() < 300) for (volatile u32 i = 0; i < 1000; i++) ;
    for (;;) {
        if (n < 3) {
            char buf[48];
            int l = ksprintf(buf, "[task A] pid %u heartbeat %u\n",
                             sched_current_pid(), n);
            serial_write(buf, (u32)l);
        }
        n++;
        for (volatile u32 i = 0; i < 3000000; i++) ;
    }
}

static void demo_task_b(void) {
    u32 n = 0;
    while (timer_ticks() < 300) for (volatile u32 i = 0; i < 1000; i++) ;
    for (;;) {
        if (n < 3) {
            char buf[48];
            int l = ksprintf(buf, "[task B] pid %u heartbeat %u\n",
                             sched_current_pid(), n);
            serial_write(buf, (u32)l);
        }
        n++;
        for (volatile u32 i = 0; i < 3000000; i++) ;
    }
}

/* load hello.elf from the FAT data disk and start it as a ring-3 process */
static void try_load_user_app(void) {
    u8 *appbuf = (u8*)kmalloc(16384);
    if (!appbuf) {
        kprintf("user: no heap space for app image\n");
        return;
    }
    u32 size = 0;
    if (fat_read("hello.elf", appbuf, 16384, &size) && size > 0) {
        u32 pid = user_spawn("user-hello", appbuf, size);
        if (pid)
            kprintf("user: loaded hello.elf from FAT disk, pid=%u\n", pid);
        else
            kprintf("user: hello.elf load failed (bad image?)\n");
    } else {
        kprintf("user: hello.elf not found (attach data.img as an IDE disk)\n");
    }
    kfree(appbuf);
}

void kernel_main(void) {
    serial_init();
    bootlog_banner();

    bootlog_start("gdt", "segments");
    gdt_init();
    bootlog_ok("5 entries");

    bootlog_start("idt", "256 gates");
    idt_init();
    bootlog_ok("int 0x80 open to ring3");

    bootlog_start("pic", "IRQ remap");
    pic_init();
    bootlog_ok("0-15 -> 32-47");

    bootlog_start("pit", "timer");
    timer_init(100);
    bootlog_ok("100 Hz");

    bootlog_start("ps2", "keyboard + mouse");
    keyboard_init();
    mouse_init();
    bootlog_ok(NULL);

    bootlog_start("mm", "physical");
    mm_init();
    bootlog_ok(NULL);

    bootlog_start("heap", "kernel");
    kheap_init();
    bootlog_ok(NULL);

    bootlog_start("paging", "per-process + COW");
    paging_init();
    bootlog_ok(NULL);

    bootlog_start("video", "VBE framebuffer");
    if (gfx_hw_init())
        bootlog_ok(NULL);
    else
        bootlog_fail("no VBE (mode 13h)");

    bootlog_start("security", "hardening");
    user_init();
    if (paging_smep_enabled())
        bootlog_ok("CR0.WP, SMEP");
    else
        bootlog_ok("CR0.WP (SMEP n/a)");

    bootlog_start("ata", "probe");
    if (ata_init())
        bootlog_ok(NULL);
    else
        bootlog_fail("no drive");

    bootlog_start("fat", "FAT12 volume");
    if (fat_mount()) {
        char note[24];
        ksprintf(note, "%u KB free", fat_free_bytes() / 1024);
        bootlog_ok(note);
    } else {
        bootlog_fail("mount failed");
    }

    bootlog_start("rtc", "CMOS clock");
    {
        char ts[56];
        if (rtc_str(ts, sizeof(ts)))
            bootlog_ok(ts);
        else
            bootlog_fail("clock not set");
    }

    bootlog_start("net", "auto-detect");
    if (net_init()) {
        static const char *H = "0123456789abcdef";
        u8 m[6];
        char mac[20];
        net_get_mac(m);
        for (int i = 0, w = 0; i < 6; i++) {
            mac[w++] = H[m[i] >> 4];
            mac[w++] = H[m[i] & 15];
            if (i < 5) mac[w++] = ':';
            mac[w] = '\0';
        }
        bootlog_ok(mac);
    } else {
        bootlog_ok("none (serial update channel)");
    }

    bootlog_start("update", "over-the-air");
    upd_init();
    bootlog_ok("UDP 9999 / serial, crc32");

    bootlog_start("sched", "round-robin");
    sched_init();
    sched_adopt_current("main");        /* the boot thread becomes task 0 */
    task_create("demo-a", demo_task_a);
    task_create("demo-b", demo_task_b);
    try_load_user_app();
    {
        char note[16];
        ksprintf(note, "%u tasks", sched_task_count());
        bootlog_ok(note);
    }

    sti();
    bootlog_done();

    vga_init();
    shell_run();
}
