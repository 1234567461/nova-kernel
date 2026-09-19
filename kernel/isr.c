/* NovaOS - common ISR dispatcher and exception reporting */
#include "idt.h"
#include "irq.h"
#include "io.h"
#include "vga.h"
#include "serial.h"
#include "printf.h"
#include "string.h"

static const char *exceptions[32] = {
    "Divide-by-zero", "Debug", "NMI", "Breakpoint", "Overflow",
    "BOUND range", "Invalid opcode", "Device not available",
    "Double fault", "Coprocessor overrun", "Invalid TSS",
    "Segment not present", "Stack-segment fault", "General protection",
    "Page fault", "Reserved", "x87 FPU error", "Alignment check",
    "Machine check", "SIMD FP exception", "Virtualization",
    "Control-protection", "Reserved", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved", "Reserved",
    "Reserved", "Reserved"
};

void isr_common_handler(struct regs *r) {
    if (r->int_no < 32) {
        /* CPU exception - report and halt */
        vga_write("\n[PANIC] Exception: ", 0x4C);
        vga_write(exceptions[r->int_no], 0x4C);
        vga_write("\n", 0x4C);
        kprintf("PANIC: %s (int %u, err 0x%x, eip 0x%x)\n",
                exceptions[r->int_no], r->int_no, r->err_code, r->eip);
        if (r->int_no == 14) {
            u32 cr2;
            __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
            kprintf("page fault address: 0x%x\n", cr2);
        }
        cli();
        for (;;) hlt();
    } else if (r->int_no >= 32 && r->int_no <= 47) {
        u8 irq = (u8)(r->int_no - 32);
        if (irq_handlers[irq])
            irq_handlers[irq](r);
        pic_eoi(irq);
    } else if (r->int_no == 128) {
        extern void syscall_dispatch(struct regs *r);
        syscall_dispatch(r);
    }
}
