/* NovaOS - GDT implementation.
 * The bootloader already installed a flat GDT; we rebuild our own here
 * so the kernel owns the descriptor tables (and can later add TSS/LDT).
 */
#include "gdt.h"
#include "io.h"
#include "serial.h"
#include "string.h"

struct gdt_entry {
    u16 limit_low;
    u16 base_low;
    u8  base_mid;
    u8  access;
    u8  granularity;
    u8  base_high;
} __attribute__((packed));

struct gdt_ptr {
    u16 limit;
    u32 base;
} __attribute__((packed));

static struct gdt_entry gdt[3];
static struct gdt_ptr   gp;

static void gdt_set_entry(int i, u32 base, u32 limit, u8 access, u8 gran) {
    gdt[i].base_low   = (u16)(base & 0xFFFF);
    gdt[i].base_mid   = (u8)((base >> 16) & 0xFF);
    gdt[i].base_high  = (u8)((base >> 24) & 0xFF);
    gdt[i].limit_low  = (u16)(limit & 0xFFFF);
    gdt[i].granularity = (u8)(((limit >> 16) & 0x0F) | (gran & 0xF0));
    gdt[i].access     = access;
}

void gdt_init(void) {
    gdt_set_entry(0, 0, 0, 0, 0);                    /* null */
    gdt_set_entry(1, 0, 0xFFFFF, 0x9A, 0xCF);        /* code, 4GB */
    gdt_set_entry(2, 0, 0xFFFFF, 0x92, 0xCF);        /* data, 4GB */
    gp.limit = (u16)(sizeof(gdt) - 1);
    gp.base  = (u32)&gdt;
    __asm__ volatile ("lgdt %0" : : "m"(gp));
    /* reload segments */
    __asm__ volatile (
        "ljmp $0x08, $1f\n\t"
        "1:\n\t"
        "mov $0x10, %%ax\n\t"
        "mov %%ax, %%ds\n\t"
        "mov %%ax, %%es\n\t"
        "mov %%ax, %%fs\n\t"
        "mov %%ax, %%gs\n\t"
        "mov %%ax, %%ss\n\t"
        : : : "ax", "memory");
}
