#include "vbe.h"
#include "serial.h"
#include "printf.h"

#define BI ((volatile nova_vbe_t*)0x8F00u)

static int  ready = 0;
static u32 *fb = 0;
static int  W = 0, H = 0, PITCH = 0;

int vbe_init(void) {
    char b[64];
    ready = 0;
    ksprintf(b, "[vbe] bootinfo magic=%x\n", (u32)BI->magic);
    serial_puts(b);
    if (BI->magic != NOVA_VBE_MAGIC) return 0;
    if (BI->bpp != 32) { serial_puts("[vbe] bpp mismatch\n"); return 0; }
    if (BI->w < 640 || BI->w > 4096 || BI->h < 480 || BI->h > 2400) {
        serial_puts("[vbe] resolution out of range\n");
        return 0;
    }
    if (BI->lfb == 0) { serial_puts("[vbe] lfb=0\n"); return 0; }

    fb = (u32*)(u32)BI->lfb;
    W  = (int)BI->w;
    H  = (int)BI->h;
    PITCH = (int)(BI->pitch / 4);
    if (PITCH < W) PITCH = W;          /* be forgiving of odd firmware */

    /* smoke test: a linear framebuffer that cannot hold one scanline is
     * worse than no VBE at all - fall back to mode 13h instead of
     * scribbling over random physical memory */
    volatile u32 *first = fb;
    first[0] = 0x101820;
    if (first[0] != 0x101820) {
        serial_puts("[vbe] lfb smoke test failed (mapping?)\n");
        fb = 0;
        return 0;
    }

    ready = 1;
    {
        char buf[96];
        ksprintf(buf, "[vbe] %ux%ux32 lfb=%x pitch=%u\n",
                 (u32)W, (u32)H, (u32)BI->lfb, (u32)BI->pitch);
        serial_puts(buf);
    }
    return 1;
}

int  vbe_ready(void)     { return ready; }
u32 *vbe_fb(void)        { return fb; }
int  vbe_width(void)     { return W; }
int  vbe_height(void)    { return H; }
int  vbe_pitch_px(void)  { return PITCH; }
