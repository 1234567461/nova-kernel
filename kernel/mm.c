/* NovaOS - physical frame allocator */
#include "mm.h"
#include "string.h"

#define BITMAP_SIZE (FRAME_COUNT / 8)
static u8 bitmap[BITMAP_SIZE] __attribute__((aligned(4)));

/* We never free kernel image pages; bitmap starts clean.  The first
 * frame (real-mode IVT/BDA) and the staging area below 1MB are marked
 * used so the allocator only hands out kernel-safe memory. */
void mm_init(void) {
    memset(bitmap, 0, sizeof(bitmap));
    /* mark everything below 1MB used (bootloader, staging, video) */
    u32 frames_below_1m = 0x100000 / FRAME_SIZE;
    for (u32 i = 0; i < frames_below_1m; i++)
        bitmap[i / 8] |= (u8)(1 << (i % 8));
}

void *frame_alloc(void) {
    for (u32 i = 0; i < FRAME_COUNT; i++) {
        if (!(bitmap[i / 8] & (1 << (i % 8)))) {
            bitmap[i / 8] |= (u8)(1 << (i % 8));
            return (void*)(i * FRAME_SIZE);
        }
    }
    return NULL;
}

void frame_free(void *addr) {
    u32 i = (u32)addr / FRAME_SIZE;
    if (i >= FRAME_COUNT) return;
    bitmap[i / 8] &= (u8)~(1 << (i % 8));
}

u32 mm_free_frames(void) {
    u32 n = 0;
    for (u32 i = 0; i < FRAME_COUNT; i++)
        if (!(bitmap[i / 8] & (1 << (i % 8)))) n++;
    return n;
}
