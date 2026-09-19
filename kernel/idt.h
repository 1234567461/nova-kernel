/* NovaOS - IDT and interrupt dispatch */
#ifndef NOVA_IDT_H
#define NOVA_IDT_H

#include "common.h"

/* interrupt context pushed by the asm stubs (see isr_stubs.asm) */
struct regs {
    u32 gs, fs, es, ds;      /* pushed by stub */
    u32 edi, esi, ebp, esp, ebx, edx, ecx, eax;  /* pushad */
    u32 int_no, err_code;    /* pushed by stub */
    u32 eip, cs, eflags, user_esp, user_ss;      /* CPU */
};

typedef void (*isr_handler_t)(struct regs *);

/* irq dispatch table (index 0..15, wired by register_irq_handler) */
extern isr_handler_t irq_handlers[16];

void idt_init(void);
void register_isr_handler(u8 n, isr_handler_t h);
void register_irq_handler(u8 irq, isr_handler_t h);
void isr_common_handler(struct regs *r);

#endif
