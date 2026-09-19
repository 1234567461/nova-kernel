/* NovaOS - userland support: TSS + minimal ELF32 loader
 *
 * Milestone 0.5: ring-3 user processes in a single address space.
 * Milestone 1.0: every process owns a page directory + page table
 * (paging_create_addr_space); the ELF loader maps the LOAD segments
 * into that private space and the scheduler switches CR3 on context
 * switch.  fork() clones the space with copy-on-write.
 */
#ifndef NOVA_USER_H
#define NOVA_USER_H

#include "common.h"

#define USER_TEXT_BASE 0x200000u   /* lowest allowed user load address */
#define USER_STACK_TOP 0x2F0000u   /* user stack grows down from here */

void user_init(void);                       /* install TSS + user GDT selectors */
u32  user_load_elf(u32 cr3, const u8 *img, u32 size); /* map + copy, returns entry */
u32  user_spawn(const char *name, const u8 *img, u32 size); /* pid or 0 */

#endif
