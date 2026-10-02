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
        /* page fault: v1.0 resolves copy-on-write writes from ring 3
         * (shared read-only user pages get copied on first write). */
        if (r->int_no == 14 && (r->err_code & 0x4)) {
            u32 cr2;
            __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
            extern int paging_cow_fault(u32 cr2, u32 err);
            if (paging_cow_fault(cr2, r->err_code))
                return;                       /* iret retries the faulting insn */
            /* COW could not resolve it - the process wrote outside its
             * mappings, so it is unrecoverable for *this* process only. */
            kprintf("user process killed: bad page fault addr 0x%x err 0x%x eip 0x%x\n",
                    cr2, r->err_code, r->eip);
            extern void sched_exit_current(void);
            sched_exit_current();             /* never returns */
        }
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
        /* hand the scheduler a pointer to this live interrupt frame so a
         * preemptive switch can save/restore it correctly */
        extern void sched_enter_irq(struct regs *r);
        sched_enter_irq(r);

        /* Acknowledge BEFORE dispatching.
         *
         * The handler is allowed to context-switch and never return here
         * (timer -> sched_tick -> switch_to).  With the EOI placed after the
         * handler call - as it used to be - that switch skipped it: the 8259
         * keeps the line latched in its ISR register and then refuses to
         * deliver it (or anything lower priority) ever again.  The switch code
         * compensated by sending its own EOI, which then produced a *second*
         * EOI for the same interrupt and rotated the priority chain.
         *
         * Exactly one EOI, before the handler runs, keeps the PIC sequence
         * correct whether or not the handler returns. */
        pic_eoi(irq);

        if (irq_handlers[irq])
            irq_handlers[irq](r);
    } else if (r->int_no == 128) {
        /* Publish this syscall's frame before dispatching.  fork() clones the
         * caller's context from it, and a timer IRQ that lands while we are
         * inside the syscall must save *this* frame - leaving the pointer at
         * the previous IRQ's frame made fork() copy a stale stack pointer and
         * the child then iret'd from the wrong place, which surfaced as a
         * general-protection fault inside switch_to. */
        extern void sched_enter_irq(struct regs *r);
        sched_enter_irq(r);
        extern void syscall_dispatch(struct regs *r);
        syscall_dispatch(r);
    }
}
