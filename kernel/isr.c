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
        /* full CPU state: a GP with err=0 at a random data-reading mov is
         * almost always a bad segment register or stack, so dump them all */
        {
            u32 cr0, cr2, cr3, esp, efl;
            u16 cs_v, ds_v, es_v, fs_v, gs_v, ss_v;
            __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
            __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
            __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
            __asm__ volatile ("mov %%esp, %0" : "=r"(esp));
            __asm__ volatile ("pushfl; popl %0" : "=r"(efl));
            __asm__ volatile ("mov %%cs, %0" : "=r"(cs_v));
            __asm__ volatile ("mov %%ds, %0" : "=r"(ds_v));
            __asm__ volatile ("mov %%es, %0" : "=r"(es_v));
            __asm__ volatile ("mov %%fs, %0" : "=r"(fs_v));
            __asm__ volatile ("mov %%gs, %0" : "=r"(gs_v));
            __asm__ volatile ("mov %%ss, %0" : "=r"(ss_v));
            kprintf("  cr0=0x%x cr2=0x%x cr3=0x%x\n", cr0, cr2, cr3);
            kprintf("  cs=0x%x ds=0x%x es=0x%x fs=0x%x gs=0x%x ss=0x%x\n",
                    cs_v, ds_v, es_v, fs_v, gs_v, ss_v);
            kprintf("  esp=0x%x eflags=0x%x\n", esp, efl);
            kprintf("  eax=0x%x ecx=0x%x edx=0x%x ebx=0x%x\n",
                    r->eax, r->ecx, r->edx, r->ebx);
            kprintf("  esi=0x%x edi=0x%x ebp=0x%x\n",
                    r->esi, r->edi, r->ebp);
            /* page-table walk for the data address in EAX too */
            {
                u32 pdi2 = (cr3 & 0xFFFFF000);
                u32 v2   = r->eax;
                u32 pde2 = *(u32*)(pdi2 + (v2 >> 22) * 4);
                if (pde2 & 1) {
                    u32 pt2 = pde2 & 0xFFFFF000;
                    kprintf("  pages[eax=0x%x]: pde=0x%x pte=0x%x\n",
                            v2, pde2, *(u32*)(pt2 + ((v2 >> 12) & 0x3FF) * 4));
                }
            }
            /* walk the page tables for the faulting eip and the kernel bss
             * page: a malformed PTE that QEMU tolerates is the prime
             * suspect for emulator-only faults */
            {
                u32 pdi = (cr3 & 0xFFFFF000);
                u32 v   = r->eip;
                u32 pdi_idx = v >> 22;
                u32 pte_idx = (v >> 12) & 0x3FF;
                u32 pde = *(u32*)(pdi + pdi_idx * 4);
                if (pde & 1) {
                    u32 pt = pde & 0xFFFFF000;
                    u32 pte = *(u32*)(pt + pte_idx * 4);
                    kprintf("  pages[eip]: pde=0x%x pte=0x%x\n", pde, pte);
                    pde = *(u32*)(pdi + (0x10D000 >> 22) * 4);
                    pt  = pde & 0xFFFFF000;
                    kprintf("  pages[bss]: pde=0x%x pte=0x%x\n",
                            pde, *(u32*)(pt + (0x10D000 >> 12 & 0x3FF) * 4));
                } else {
                    kprintf("  pages[eip]: pde=0x%x NOT PRESENT\n", pde);
                }
            }
        }
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
