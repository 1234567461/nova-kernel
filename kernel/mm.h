/* NovaOS - physical frame allocator (bitmap) */
#ifndef NOVA_MM_H
#define NOVA_MM_H

#include "common.h"

#define FRAME_SIZE  0x1000          /* 4KB */
#define FRAME_COUNT (MEM_END / FRAME_SIZE)   /* 4MB -> 1024 frames */

void    mm_init(void);
void   *frame_alloc(void);          /* returns 4KB-aligned address or NULL */
void    frame_free(void *addr);
u32     mm_free_frames(void);

#endif
