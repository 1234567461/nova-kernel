/* NovaOS - GDT setup */
#ifndef NOVA_GDT_H
#define NOVA_GDT_H

#include "common.h"

void gdt_init(void);

/* selectors */
#define GDT_CODE_SEL 0x08
#define GDT_DATA_SEL 0x10

#endif
