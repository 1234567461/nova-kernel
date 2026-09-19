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
#include "mm.h"
#include "kheap.h"
#include "paging.h"
#include "sched.h"
#include "shell.h"
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

void kernel_main(void) {
    serial_init();
    kprintf("NovaOS kernel v0.1 booting...\n");

    gdt_init();
    kprintf("gdt: ok\n");

    idt_init();
    kprintf("idt: ok\n");

    pic_init();
    kprintf("pic: ok (IRQ remap 32-47)\n");

    timer_init(100);
    kprintf("pit: ok (100Hz)\n");

    keyboard_init();
    kprintf("ps2: ok\n");

    mm_init();
    kheap_init();
    kprintf("memory: phys free %u KB, heap 0x%x-0x%x\n",
            mm_free_frames() * 4, HEAP_START, HEAP_END);

    paging_init();
    kprintf("paging: 4MB identity map, cr3=0x%x\n", paging_get_cr3());

    sched_init();
    task_create("demo-a", demo_task_a);
    task_create("demo-b", demo_task_b);
    kprintf("sched: %u tasks ready\n", sched_task_count());

    sti();

    vga_init();
    vga_write("\n", 0x0F);
    vga_write("===============================================\n", 0x09);
    vga_write("  NovaOS v0.1 - self-made operating system\n", 0x0F);
    vga_write("  boot: stage1 -> stage2 -> pmode -> paging\n", 0x0F);
    vga_write("  type 'help' for commands, 'gui' for desktop\n", 0x0F);
    vga_write("===============================================\n", 0x09);
    vga_write("\n", 0x0F);

    shell_run();
}
