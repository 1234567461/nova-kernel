/* NovaOS - VGA display driver
 * Two backends:
 *   1. 80x25 text mode  (0xB8000), used for boot console and shell
 *   2. 320x200x256 mode-13h graphics, used by the built-in GUI
 * The graphics mode is set by writing VGA registers directly (no VBE).
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

/* graphics mode */
void    vga_set_mode13h(void);
void    gfx_putpixel(int x, int y, u8 color);
void    gfx_fillrect(int x0, int y0, int x1, int y1, u8 color);
void    gfx_drawchar(int x, int y, char c, u8 fg, u8 bg);
void    gfx_drawstring(int x, int y, const char *s, u8 fg, u8 bg);
void    gfx_clear(u8 color);

#endif
