/* NovaOS - paging: identity map first 4MB (one page table). */
#include "paging.h"
#include "string.h"

#define PAGE_DIR   0x9000u
#define PAGE_TABLE 0xA000u
#define PAGE_COUNT (MEM_END / 0x1000)   /* 1024 pages for 4MB */

void paging_init(void) {
    /* page directory: one entry pointing at our page table */
    memset((void*)PAGE_DIR, 0, 0x1000);
    memset((void*)PAGE_TABLE, 0, 0x1000);

    u32 *pt = (u32*)PAGE_TABLE;
    for (u32 i = 0; i < PAGE_COUNT; i++)
        pt[i] = (i * 0x1000) | 0x3;      /* present | writable */

    u32 *pd = (u32*)PAGE_DIR;
    pd[0] = PAGE_TABLE | 0x3;

    __asm__ volatile ("mov %0, %%cr3" : : "r"((u32)PAGE_DIR));
    u32 cr0;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 0x80000000;                   /* PG */
    __asm__ volatile ("mov %0, %%cr0" : : "r"(cr0));
}

u32 paging_get_cr3(void) {
    u32 cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}
