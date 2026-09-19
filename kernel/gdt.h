/* NovaOS - GDT setup */
#ifndef NOVA_GDT_H
#define NOVA_GDT_H

#include "common.h"

void gdt_init(void);

/* selectors */
#define GDT_CODE_SEL  0x08
#define GDT_DATA_SEL  0x10
#define GDT_UCODE_SEL 0x1B
#define GDT_UDATA_SEL 0x23
#define GDT_TSS_SEL   0x28

void gdt_set_tss(u32 base, u32 limit);   /* install the TSS descriptor */

#endif
