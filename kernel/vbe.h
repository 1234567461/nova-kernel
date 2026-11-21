/* NovaOS - VBE linear framebuffer
 *
 * The stage-2 loader probes VBE in real mode (the only place the BIOS
 * video services still exist), programs a 32bpp linear-framebuffer mode
 * and publishes the result as a struct at physical 0x8F00:
 *
 *      0x8F00 u32 magic  'VBE1' (0 = firmware had no usable mode)
 *      0x8F04 u32 width
 *      0x8F08 u32 height
 *      0x8F0C u32 pitch  (bytes per scanline)
 *      0x8F10 u32 bpp
 *      0x8F14 u32 framebuffer physical address
 *
 * The loader also maps the framebuffer with one 4MB page directory entry,
 * so the physical address is directly usable from the kernel.
 */
#ifndef VBE_H
#define VBE_H

#include "common.h"

typedef struct {
    u32 magic;
    u32 w, h, pitch, bpp, lfb;
} nova_vbe_t;

#define NOVA_VBE_MAGIC 0x31454256u   /* 'VBE1' */

/* returns 1 when a true-colour framebuffer is available */
int  vbe_init(void);
int  vbe_ready(void);
u32 *vbe_fb(void);
int  vbe_width(void);
int  vbe_height(void);
int  vbe_pitch_px(void);            /* stride in 32-bit pixels */

#endif
