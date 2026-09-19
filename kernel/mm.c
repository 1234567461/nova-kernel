/* NovaOS - physical frame allocator */
#include "mm.h"
#include "string.h"

#define BITMAP_SIZE (FRAME_COUNT / 8)
static u8 bitmap[BITMAP_SIZE] __attribute__((aligned(4)));

/* user-page refcounts (COW sharing); 0 = not shared */
static u8 frame_refs[FRAME_COUNT];

static void mark_used(u32 fi) {
    if (fi < FRAME_COUNT) bitmap[fi / 8] |= (u8)(1 << (fi % 8));
}

/* Reserved regions:
 *   - below 1MB: real-mode IVT/BDA, bootloader staging, VGA memory
 *   - 0x100000-0x130000: kernel image + stacks (conservative bound)
 *   - 0x300000-0x400000: kernel heap arena (managed by kheap)
 * Frames 0x130000-0x2FFFFF (~464 frames, ~1.8MB) are handed out for
 * page tables and user pages.
 */
void mm_init(void) {
    memset(bitmap, 0, sizeof(bitmap));
    memset(frame_refs, 0, sizeof(frame_refs));
    for (u32 i = 0; i < 0x100000 / FRAME_SIZE; i++)                mark_used(i);
    for (u32 i = 0x100000 / FRAME_SIZE; i < 0x130000 / FRAME_SIZE; i++) mark_used(i);
    for (u32 i = 0x300000 / FRAME_SIZE; i < FRAME_COUNT; i++)      mark_used(i);
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

void mm_ref_inc(u32 fi) {
    if (fi < FRAME_COUNT && frame_refs[fi] < 255) frame_refs[fi]++;
}

void mm_ref_dec(u32 fi) {
    if (fi >= FRAME_COUNT) return;
    if (frame_refs[fi] > 0) frame_refs[fi]--;
    /* last reference gone: give the frame back to the allocator */
    if (frame_refs[fi] == 0 && (bitmap[fi / 8] & (1 << (fi % 8))))
        frame_free((void*)(fi * FRAME_SIZE));
}
