/* NovaOS - userland support implementation */
#include "user.h"
#include "gdt.h"
#include "sched.h"
#include "string.h"
#include "printf.h"
#include "serial.h"

/* ------------------------------------------------------------------ */
/* TSS: lets the CPU find the ring-0 stack when a ring-3 program is    */
/* interrupted (IRQ or int 0x80).                                      */
/* ------------------------------------------------------------------ */
struct tss {
    u32 prev, esp0, ss0, esp1, ss1, esp2, ss2;
    u32 cr3, eip, eflags, eax, ecx, edx, ebx;
    u32 esp, ebp, esi, edi;
    u32 es, cs, ss, ds, fs, gs, ldt;
    u16 trap, iomap;
} __attribute__((packed));

static struct tss tss;

extern u32 sys_stack_top;          /* defined in link.ld */

void user_init(void) {
    memset(&tss, 0, sizeof(tss));
    tss.ss0  = GDT_DATA_SEL;
    tss.esp0 = sys_stack_top;
    gdt_set_tss((u32)&tss, (u32)sizeof(tss) - 1);
    __asm__ volatile ("ltr %0" : : "r"((u16)GDT_TSS_SEL));
    kprintf("user: TSS ready (ring0 stack 0x%x, ring3 supported)\n", tss.esp0);
}

/* ------------------------------------------------------------------ */
/* Minimal ELF32 loader (ET_EXEC only, EM_386).  Loads PT_LOAD         */
/* segments into the user window and returns the entry point.          */
/* ------------------------------------------------------------------ */
struct elf32_hdr {
    u8  e_ident[16];
    u16 e_type, e_machine;
    u32 e_version, e_entry, e_phoff, e_shoff, e_flags;
    u16 e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
} __attribute__((packed));

struct elf32_phdr {
    u32 p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align;
} __attribute__((packed));

#define PT_LOAD 1

u32 user_load_elf(const u8 *img, u32 size) {
    if (size < sizeof(struct elf32_hdr)) return 0;
    const struct elf32_hdr *h = (const struct elf32_hdr*)img;
    if (h->e_ident[0] != 0x7F || h->e_ident[1] != 'E' ||
        h->e_ident[2] != 'L'  || h->e_ident[3] != 'F') return 0;
    if (h->e_machine != 3) return 0;                 /* EM_386 */
    if (h->e_type != 2)    return 0;                 /* ET_EXEC */
    if (h->e_phoff + h->e_phnum * sizeof(struct elf32_phdr) > size) return 0;

    const struct elf32_phdr *ph =
        (const struct elf32_phdr*)(img + h->e_phoff);
    for (u16 i = 0; i < h->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        if (ph[i].p_vaddr < USER_TEXT_BASE) return 0;
        if (ph[i].p_vaddr + ph[i].p_memsz > USER_STACK_TOP) return 0;
        if (ph[i].p_offset + ph[i].p_filesz > size) return 0;
        memcpy((void*)ph[i].p_vaddr, img + ph[i].p_offset, ph[i].p_filesz);
        if (ph[i].p_memsz > ph[i].p_filesz)
            memset((void*)(ph[i].p_vaddr + ph[i].p_filesz), 0,
                   ph[i].p_memsz - ph[i].p_filesz);
    }
    return h->e_entry;
}

u32 user_spawn(const char *name, const u8 *img, u32 size) {
    u32 entry = user_load_elf(img, size);
    if (!entry) return 0;
    return task_create_user(name, entry);
}
