/* NovaOS - userland support: TSS + minimal ELF32 loader
 *
 * Milestone 0.5: ring-3 user processes.  The kernel still uses a single
 * address space, but the page tables now mark kernel memory supervisor-only
 * (user pages are marked U/S).  A user program is loaded as an ELF32 image
 * into the user window 0x200000..0x2FFFFF and entered in ring 3 via a
 * scheduler frame (iret with user CS/SS).  Interrupts from ring 3 switch
 * to the kernel through the TSS (ss0/esp0 -> sys_stack_top).
 */
#ifndef NOVA_USER_H
#define NOVA_USER_H

#include "common.h"

#define USER_TEXT_BASE 0x200000u   /* lowest allowed user load address */
#define USER_STACK_TOP 0x2F0000u   /* user stack grows down from here */

void user_init(void);                       /* install TSS + user GDT selectors */
u32  user_load_elf(const u8 *img, u32 size);/* returns entry point or 0 */
u32  user_spawn(const char *name, const u8 *img, u32 size); /* pid or 0 */

#endif
