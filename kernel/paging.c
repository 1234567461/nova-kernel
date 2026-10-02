/* NovaOS - paging: per-process address spaces + copy-on-write (v1.0)
 *
 * v0.5 used a single page table for the whole 4MB identity map.  v1.0
 * gives every user process its own page directory + page table:
 *   - the kernel region (0x000000-0x1FFFFF image/lowmem, 0x300000-0x3FFFFF
 *     heap) is inherited by copying the kernel PTEs (same physical frames)
 *   - the user window 0x200000-0x2FFFFF is private to each process
 *   - fork() clones the address space with copy-on-write: user PTEs are
 *     shared read-only until one side writes, which raises #PF (page
 *     fault) and paging_cow_fault() copies the frame on demand
 */
#include "paging.h"
#include "mm.h"
#include "string.h"
#include "serial.h"

#define USER_PG_START 512              /* 0x200000 */
#define USER_PG_END   768              /* 0x300000 */

#define PTE_P   0x1
#define PTE_W   0x2
#define PTE_U   0x4

static int smep_on = 0;

void paging_init(void) {
    /* KERNEL_PD (0x9000) / KERNEL_PT (0xA000) are *already live*: the
     * stage-2 bootloader built exactly this identity map and set CR3 to it
     * before jumping here.  So we must NOT memset either structure - zeroing
     * the active page directory (or page table) unmaps the code currently
     * executing and the stack it is using, which faults on the very next
     * instruction fetch and escalates #PF -> #DF -> triple fault.
     *
     * Instead we rewrite the page-table entries in place.  Every entry below
     * the user window is written with the same identity mapping the loader
     * installed, so the running code (0x100000+) and the kernel stack
     * (0x10F000) keep their translations throughout.  Only the user window
     * entries (0x200000-0x2FFFFF, pages 512..767), which are unused until a
     * process is created, are cleared.
     */
    u32 *pt = (u32*)KERNEL_PT;
    u32 *pd = (u32*)KERNEL_PD;

    for (u32 i = 0; i < 1024; i++) {
        if (i >= USER_PG_START && i < USER_PG_END)
            pt[i] = 0;                 /* user window: per-process */
        else
            pt[i] = (i * 0x1000) | 0x3;
    }

    /* keep PDE[0] pointing at the kernel page table (present + rw + user) */
    pd[0] = KERNEL_PT | 0x7;

    paging_switch(KERNEL_PD);
    u32 cr0;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 0x80000000;                 /* PG */
    __asm__ volatile ("mov %0, %%cr0" : : "r"(cr0));

    paging_harden_cpu();
}

/* Harden the MMU against the classic ring-3 attacks.
 *
 *  CR0.WP  - the kernel may no longer write through read-only PTEs.  Without
 *            it a kernel store into a user page flagged read-only silently
 *            succeeded, which defeats copy-on-write and would let a bug (or a
 *            crafted syscall) modify a page it had just declared immutable.
 *
 *  CR4.SMEP - Supervisor Mode Execution Prevention.  The kernel faults if it
 *            ever tries to *execute* a user page.  This closes the whole
 *            family of "jump to a ring-3 buffer" attacks: a syscall that is
 *            tricked into calling a function pointer it read from user
 *            memory now dies instead of running attacker code at ring 0.
 *
 * Both features are probed through CPUID and skipped when absent, so the
 * kernel still boots on an emulator or CPU that does not implement them. */
void paging_harden_cpu(void) {
    u32 cr0;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 0x00010000;                 /* WP */
    __asm__ volatile ("mov %0, %%cr0" : : "r"(cr0));

    /* CPUID leaf 7, sub-leaf 0, EBX bit 7 = SMEP */
    u32 max_leaf;
    __asm__ volatile ("cpuid" : "=a"(max_leaf) : "a"(0) : "ebx", "ecx", "edx");
    if (max_leaf >= 7) {
        u32 a, b, c, d;
        __asm__ volatile ("cpuid"
                          : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                          : "a"(7), "c"(0));
        if (b & (1u << 7)) {
            u32 cr4;
            __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
            cr4 |= (1u << 20);         /* SMEP */
            __asm__ volatile ("mov %0, %%cr4" : : "r"(cr4));
            smep_on = 1;
        }
    }
    serial_puts("[sec] CR0.WP on");
    if (smep_on) serial_puts(", CR4.SMEP on");
    else         serial_puts(" (SMEP not available on this CPU)");
    serial_puts("\n");
}

int paging_smep_enabled(void) { return smep_on; }

void paging_switch(u32 cr3) {
    __asm__ volatile ("mov %0, %%cr3" : : "r"(cr3));
}

u32 paging_get_cr3(void) {
    u32 cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}

/* map one user page inside the address space identified by cr3 */
static int map_user_page(u32 pt, u32 idx) {
    u32 *pte = (u32*)(pt + idx * 4);
    if (*pte & PTE_P) return 1;        /* already mapped */
    u32 f = (u32)frame_alloc();
    if (!f) return 0;
    *pte = f | (PTE_P | PTE_W | PTE_U);
    mm_ref_inc(f >> 12);
    return 1;
}

/* ------------------------------------------------------------------ */
/* address spaces                                                      */
/* ------------------------------------------------------------------ */

u32 paging_create_addr_space(void) {
    u32 pd = (u32)frame_alloc();
    u32 pt = (u32)frame_alloc();
    if (!pd || !pt) {
        if (pd) frame_free((void*)pd);
        if (pt) frame_free((void*)pt);
        return 0;
    }
    memcpy((void*)pt, (void*)KERNEL_PT, 0x1000);   /* inherit kernel map */
    u32 *p = (u32*)pt;
    for (u32 i = USER_PG_START; i < USER_PG_END; i++) p[i] = 0;
    memcpy((void*)pd, (void*)KERNEL_PD, 0x1000);
    ((u32*)pd)[0] = pt | 0x7;
    return pd;
}

u32 paging_fork_addr_space(u32 parent_cr3) {
    u32 ppde = ((u32*)parent_cr3)[0];
    if (!(ppde & PTE_P)) return 0;
    u32 ppt = ppde & 0xFFFFF000;

    u32 pd = (u32)frame_alloc();
    u32 pt = (u32)frame_alloc();
    if (!pd || !pt) {
        if (pd) frame_free((void*)pd);
        if (pt) frame_free((void*)pt);
        return 0;
    }
    memcpy((void*)pd, (void*)parent_cr3, 0x1000);
    memcpy((void*)pt, (void*)ppt, 0x1000);

    /* COW: share every user page read-only; one extra reference each */
    u32 *pp = (u32*)ppt, *cp = (u32*)pt;
    for (u32 i = USER_PG_START; i < USER_PG_END; i++) {
        if (pp[i] & PTE_P) {
            cp[i] = pp[i] & ~(u32)PTE_W;   /* child PTE: read-only */
            pp[i] &= ~(u32)PTE_W;          /* parent PTE: read-only too */
            mm_ref_inc(pp[i] >> 12);
        }
    }
    ((u32*)pd)[0] = pt | 0x7;

    /* CRITICAL: the parent's page table entries were just downgraded from
     * writable to read-only, but its TLB still holds the old writable
     * translations.  Without a flush the parent keeps writing the shared
     * page directly - no COW fault ever fires - and it silently stomps the
     * copy the child is about to run on.  That corruption is what made a
     * freshly forked child resume at a stale return address and land in
     * whatever code happened to live there.  Reload the parent's CR3 (when
     * it is the address space currently loaded) to drop the stale entries. */
    if (parent_cr3 == paging_get_cr3()) {
        __asm__ volatile ("mov %%cr3, %%eax\n\t"
                          "mov %%eax, %%cr3" : : : "eax", "memory");
    }
    return pd;
}

void paging_destroy_addr_space(u32 cr3) {
    if (cr3 == KERNEL_PD) return;
    u32 *pde = (u32*)cr3;
    for (int d = 0; d < 1024; d++) {
        if (!(pde[d] & PTE_P)) continue;
        u32 pt = pde[d] & 0xFFFFF000;

        /* Only tear down page tables this process actually owns.
         *
         * paging_create_addr_space() copies KERNEL_PD wholesale, so every
         * unused directory entry points straight at the shared kernel page
         * table (KERNEL_PT) and - because the copy is per-entry, not
         * per-address-space - a naive loop frees KERNEL_PT itself.  That
         * unmaps the whole kernel and the machine dies a few instructions
         * later.  A directory entry is ours only if it does not alias one of
         * the kernel's own tables. */
        if (pt == KERNEL_PT || pt == KERNEL_PD) continue;

        u32 *pte = (u32*)pt;
        for (u32 i = USER_PG_START; i < USER_PG_END; i++)
            if (pte[i] & PTE_P) mm_ref_dec(pte[i] >> 12);
        frame_free((void*)pt);
    }
    frame_free((void*)cr3);
}

int paging_map_user_region(u32 cr3, u32 vaddr, u32 nbytes) {
    if (nbytes == 0) return 1;
    u32 pde = ((u32*)cr3)[(vaddr >> 22) & 0x3FF];
    if (!(pde & PTE_P)) return 0;
    u32 pt = pde & 0xFFFFF000;
    u32 start = (vaddr >> 12) & 0x3FF;
    u32 end   = ((vaddr + nbytes - 1) >> 12) & 0x3FF;
    for (u32 i = start; i <= end; i++)
        if (!map_user_page(pt, i)) return 0;
    return 1;
}

/* ------------------------------------------------------------------ */
/* copy-on-write page fault                                            */
/* ------------------------------------------------------------------ */

int paging_cow_fault(u32 cr2, u32 err) {
    if (!(err & PTE_U)) return 0;      /* not from ring 3 */
    if (!(err & PTE_W)) return 0;      /* not a write */
    u32 cr3 = paging_get_cr3();
    u32 *pde = (u32*)(cr3 + (((cr2 >> 22) & 0x3FF) * 4));
    if (!(*pde & PTE_P)) return 0;
    u32 pt = *pde & 0xFFFFF000;
    u32 *pte = (u32*)(pt + (((cr2 >> 12) & 0x3FF) * 4));
    if (!(*pte & PTE_P)) return 0;
    if (*pte & PTE_W) return 0;        /* already writable: not COW */

    u32 old = *pte & 0xFFFFF000;
    u32 nf = (u32)frame_alloc();
    if (!nf) return 0;
    memcpy((void*)nf, (void*)old, 0x1000);
    *pte = nf | (PTE_P | PTE_W | PTE_U);
    mm_ref_dec(old >> 12);
    mm_ref_inc(nf >> 12);
    __asm__ volatile ("invlpg (%0)" : : "r"(cr2));
    return 1;
}
