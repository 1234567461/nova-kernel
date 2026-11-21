/* NovaOS - display driver
 * Backends:
 *   1. 80x25 text mode (0xB8000) - boot console and shell
 *   2. VBE 32bpp linear framebuffer - the modern desktop (1024x768+)
 *   3. 320x200x256 mode 13h - last-resort fallback when no VBE exists
 */
#ifndef NOVA_VGA_H
#define NOVA_VGA_H

#include "common.h"

#define VGA_TEXT_MEM  ((volatile u16*)0xB8000)
#define VGA_WIDTH     80
#define VGA_HEIGHT    25
#define VGA_FB        ((volatile u8*)0xA0000)   /* mode-13h framebuffer */
#define GFX_W         320
#define GFX_H         200

/* text mode */
void    vga_init(void);
void    vga_clear(u8 attr);
void    vga_putc(char c, u8 attr);
void    vga_write(const char *s, u8 attr);
void    vga_set_cursor(int x, int y);
int     vga_get_x(void);
int     vga_get_y(void);
void    vga_scroll(void);

/* legacy mode-13h */
void    vga_set_mode13h(void);
void    gfx_putpixel(int x, int y, u8 color);
void    gfx_fillrect(int x0, int y0, int x1, int y1, u8 color);
void    gfx_drawchar(int x, int y, char c, u8 fg, u8 bg);
void    gfx_drawstring(int x, int y, const char *s, u8 fg, u8 bg);
void    gfx_clear(u8 color);

/* ---- true colour (VBE) --------------------------------------------- */
#define RGB(r,g,b) (((u32)(r) << 16) | ((u32)(g) << 8) | (u32)(b))

int  gfx_hw_init(void);            /* 1 = VBE framebuffer is live */
int  gfx_is_vbe(void);
int  gfx_screen_w(void);
int  gfx_screen_h(void);

void gfx_px(int x, int y, u32 rgb);
void gfx_rect(int x0, int y0, int x1, int y1, u32 rgb);
void gfx_rect_a(int x0, int y0, int x1, int y1, u32 rgb, int a);
void gfx_round(int x0, int y0, int x1, int y1, int r, u32 rgb);
void gfx_round_a(int x0, int y0, int x1, int y1, int r, u32 rgb, int a);
void gfx_grad(int x0, int y0, int x1, int y1, u32 top, u32 bot);
void gfx_ring(int x0, int y0, int x1, int y1, int r, u32 rgb);
void gfx_char(int x, int y, char c, u32 fg, int scale);
void gfx_str(int x, int y, const char *s, u32 fg, int scale);
int  gfx_strw(const char *s, int scale);
int  gfx_strh(int scale);

#endif
