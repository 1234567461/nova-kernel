/* NovaOS - kernel heap: first-fit allocator over a static region */
#include "kheap.h"
#include "mm.h"
#include "string.h"
#include "serial.h"
#include "printf.h"

typedef struct block {
    u32 size;               /* payload bytes (multiple of 8) */
    u32 free;               /* 1 = free */
    struct block *next;
} block_t;

static block_t *heap_head = NULL;

void kheap_init(void) {
    heap_head = (block_t*)HEAP_START;
    heap_head->size = HEAP_END - HEAP_START - sizeof(block_t);
    heap_head->free = 1;
    heap_head->next = NULL;
}

static void heap_split(block_t *b, u32 need) {
    /* split b into a used block + a new free remainder */
    u32 total = b->size;
    if (total - need <= sizeof(block_t) + 8) return;
    block_t *rest = (block_t*)((u8*)b + sizeof(block_t) + need);
    rest->size = total - need - sizeof(block_t);
    rest->free = 1;
    rest->next = b->next;
    b->size = need;
    b->next = rest;
}

void *kmalloc(u32 size) {
    if (size == 0) size = 8;
    size = (size + 7) & ~7u;
    for (block_t *b = heap_head; b; b = b->next) {
        if (b->free && b->size >= size) {
            heap_split(b, size);
            b->free = 0;
            return (void*)((u8*)b + sizeof(block_t));
        }
    }
    kprintf("kmalloc: out of memory (%u bytes)\n", size);
    return NULL;
}

void kfree(void *ptr) {
    if (!ptr) return;
    block_t *b = (block_t*)((u8*)ptr - sizeof(block_t));
    b->free = 1;
    /* coalesce with next */
    while (b->next && b->next->free) {
        b->size += sizeof(block_t) + b->next->size;
        b->next = b->next->next;
    }
}

u32 kheap_used(void) {
    u32 used = 0;
    for (block_t *b = heap_head; b; b = b->next)
        if (!b->free) used += b->size;
    return used;
}
