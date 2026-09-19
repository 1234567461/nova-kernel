/* NovaOS - paging: per-process address spaces + copy-on-write (v1.0) */
#ifndef NOVA_PAGING_H
#define NOVA_PAGING_H

#include "common.h"

#define KERNEL_PD  0x9000u        /* kernel page directory (phys) */
#define KERNEL_PT  0xA000u        /* kernel page table (phys) */

void paging_init(void);
u32  paging_get_cr3(void);
void paging_switch(u32 cr3);

/* per-process address spaces -------------------------------------------- */
u32  paging_create_addr_space(void);        /* new empty user window */
u32  paging_fork_addr_space(u32 parent_cr3);/* COW clone; returns child cr3 */
void paging_destroy_addr_space(u32 cr3);    /* release user pages + tables */
int  paging_map_user_region(u32 cr3, u32 vaddr, u32 nbytes);

/* page-fault hook: handles a COW write; returns 1 if resolved */
int  paging_cow_fault(u32 cr2, u32 err);

#endif
