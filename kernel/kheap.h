/* NovaOS - kernel heap (first-fit, block headers) */
#ifndef NOVA_KHEAP_H
#define NOVA_KHEAP_H

#include "common.h"

#define HEAP_START 0x300000u
#define HEAP_END   0x400000u

void    kheap_init(void);
void   *kmalloc(u32 size);
void    kfree(void *ptr);
u32     kheap_used(void);

#endif
