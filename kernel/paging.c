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

#define USER_PG_START 512              /* 0x200000 */
#define USER_PG_END   768              /* 0x300000 */

#define PTE_P   0x1
#define PTE_W   0x2
#define PTE_U   0x4

void paging_init(void) {
    memset((void*)KERNEL_PD, 0, 0x1000);
    memset((void*)KERNEL_PT, 0, 0x1000);

    u32 *pt = (u32*)KERNEL_PT;
    for (u32 i = 0; i < 1024; i++) {
        if (i >= USER_PG_START && i < USER_PG_END)
            pt[i] = 0;                 /* user window: per-process */
        else
            pt[i] = (i * 0x1000) | 0x3;
    }

    u32 *pd = (u32*)KERNEL_PD;
    pd[0] = KERNEL_PT | 0x7;

    paging_switch(KERNEL_PD);
    u32 cr0;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 0x80000000;                 /* PG */
    __asm__ volatile ("mov %0, %%cr0" : : "r"(cr0));
}

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
    return pd;
}

void paging_destroy_addr_space(u32 cr3) {
    if (cr3 == KERNEL_PD) return;
    u32 *pde = (u32*)cr3;
    for (int d = 0; d < 1024; d++) {
        if (!(pde[d] & PTE_P)) continue;
        u32 pt = pde[d] & 0xFFFFF000;
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
