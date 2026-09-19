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
#include "string.h"
#include "printf.h"

/* demo kernel threads */
static void demo_task_a(void) {
    u32 n = 0;
    for (;;) {
        char buf[48];
        ksprintf(buf, "[task A] pid %u heartbeat %u\n",
                 sched_current_pid(), n++);
        vga_write(buf, 0x0A);
        for (volatile u32 i = 0; i < 3000000; i++) ;
    }
}

static void demo_task_b(void) {
    u32 n = 0;
    for (;;) {
        char buf[48];
        ksprintf(buf, "[task B] pid %u heartbeat %u\n",
                 sched_current_pid(), n++);
        vga_write(buf, 0x0E);
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
        kprintf("user: hello.elf not found (attach data.img with -fdb)\n");
    }
    kfree(appbuf);
}

void kernel_main(void) {
    serial_init();
    kprintf("NovaOS kernel v0.5 booting...\n");

    gdt_init();
    kprintf("gdt: ok\n");

    idt_init();
    kprintf("idt: ok (int 0x80 open to ring3)\n");

    pic_init();
    kprintf("pic: ok (IRQ remap 32-47)\n");

    timer_init(100);
    kprintf("pit: ok (100Hz)\n");

    keyboard_init();
    mouse_init();
    kprintf("ps2: ok (keyboard + mouse)\n");

    mm_init();
    kheap_init();
    kprintf("memory: phys free %u KB, heap 0x%x-0x%x\n",
            mm_free_frames() * 4, HEAP_START, HEAP_END);

    paging_init();
    kprintf("paging: 4MB map, user window 0x200000-0x2FFFFF U/S\n");

    user_init();

    if (ata_init())
        kprintf("ata: secondary master present\n");
    else
        kprintf("ata: no secondary drive (run with -fdb build/data.img)\n");

    if (fat_mount())
        kprintf("fat: FAT12 data disk mounted\n");
    else
        kprintf("fat: mount failed\n");

    sched_init();
    task_create("demo-a", demo_task_a);
    task_create("demo-b", demo_task_b);
    try_load_user_app();
    kprintf("sched: %u tasks ready\n", sched_task_count());

    sti();

    vga_init();
    vga_write("\n", 0x0F);
    vga_write("===============================================\n", 0x09);
    vga_write("  NovaOS v0.5 - self-made operating system\n", 0x0F);
    vga_write("  boot: stage1 -> stage2 -> pmode -> paging\n", 0x0F);
    vga_write("  ring3 userland + FAT12 disk + mouse GUI\n", 0x0F);
    vga_write("  type 'help', 'gui', 'ls', 'cat', 'run'\n", 0x0F);
    vga_write("===============================================\n", 0x09);
    vga_write("\n", 0x0F);

    shell_run();
}
