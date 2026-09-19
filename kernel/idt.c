/* NovaOS - IDT implementation */
#include "idt.h"
#include "io.h"
#include "string.h"
#include "serial.h"

struct idt_entry {
    u16 base_low;
    u16 sel;
    u8  zero;
    u8  flags;
    u16 base_high;
} __attribute__((packed));

struct idt_ptr {
    u16 limit;
    u32 base;
} __attribute__((packed));

static struct idt_entry idt[256];
static struct idt_ptr   ip;

static isr_handler_t isr_handlers[256];
isr_handler_t irq_handlers[16];   /* referenced by isr.c via idt.h */

extern void isr0(void);   extern void isr1(void);
extern void isr2(void);   extern void isr3(void);
extern void isr4(void);   extern void isr5(void);
extern void isr6(void);   extern void isr7(void);
extern void isr8(void);   extern void isr9(void);
extern void isr10(void);  extern void isr11(void);
extern void isr12(void);  extern void isr13(void);
extern void isr14(void);  extern void isr15(void);
extern void isr16(void);  extern void isr17(void);
extern void isr18(void);  extern void isr19(void);
extern void isr20(void);  extern void isr21(void);
extern void isr22(void);  extern void isr23(void);
extern void isr24(void);  extern void isr25(void);
extern void isr26(void);  extern void isr27(void);
extern void isr28(void);  extern void isr29(void);
extern void isr30(void);  extern void isr31(void);
extern void isr32(void);  extern void isr33(void);
extern void isr34(void);  extern void isr35(void);
extern void isr36(void);  extern void isr37(void);
extern void isr38(void);  extern void isr39(void);
extern void isr40(void);  extern void isr41(void);
extern void isr42(void);  extern void isr43(void);
extern void isr44(void);  extern void isr45(void);
extern void isr46(void);  extern void isr47(void);
extern void isr128(void);

static void idt_set_entry(u8 n, u32 base, u16 sel, u8 flags) {
    idt[n].base_low  = (u16)(base & 0xFFFF);
    idt[n].base_high = (u16)((base >> 16) & 0xFFFF);
    idt[n].sel       = sel;
    idt[n].zero      = 0;
    idt[n].flags     = flags;
}

void register_isr_handler(u8 n, isr_handler_t h) { isr_handlers[n] = h; }
void register_irq_handler(u8 irq, isr_handler_t h) {
    if (irq < 16) irq_handlers[irq] = h;
}

void idt_init(void) {
    memset(idt, 0, sizeof(idt));
    memset(isr_handlers, 0, sizeof(isr_handlers));
    memset(irq_handlers, 0, sizeof(irq_handlers));

    void *stubs[] = {
        isr0,isr1,isr2,isr3,isr4,isr5,isr6,isr7,isr8,isr9,isr10,isr11,
        isr12,isr13,isr14,isr15,isr16,isr17,isr18,isr19,isr20,isr21,isr22,
        isr23,isr24,isr25,isr26,isr27,isr28,isr29,isr30,isr31,
        isr32,isr33,isr34,isr35,isr36,isr37,isr38,isr39,isr40,isr41,isr42,
        isr43,isr44,isr45,isr46,isr47, isr128
    };
    int n = sizeof(stubs) / sizeof(stubs[0]);
    for (int i = 0; i < n; i++) {
        u8 vec = (i < 48) ? (u8)i : 128;
        /* ring0 int gate; vector 128 (int 0x80) is opened to ring 3 */
        u8 flags = (vec == 128) ? 0xEE : 0x8E;
        idt_set_entry(vec, (u32)stubs[i], 0x08, flags);
    }

    ip.limit = (u16)(sizeof(idt) - 1);
    ip.base  = (u32)&idt;
    __asm__ volatile ("lidt %0" : : "m"(ip));
}
