/* NovaOS - VGA driver implementation */
#include "vga.h"
#include "io.h"
#include "string.h"
#include "serial.h"

static int tx = 0, ty = 0;
static u8 cur_attr = 0x0F;

/* 8x8 bitmap font covering all printable ASCII (0x20-0x7E).
 * Based on the public-domain font8x8 by Daniel Hepper (dhepper/font8x8);
 * indexes outside the printable range stay blank.  The old table only had
 * uppercase A-Z, so every lowercase letter, digit and punctuation mark
 * rendered as an empty cell - window titles like "Console" came out as a
 * lone "C". */
static const u8 font8x8[128][8] = {
    [' '] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    ['!'] = {0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00},
    ['"'] = {0x36,0x36,0x00,0x00,0x00,0x00,0x00,0x00},
    ['#'] = {0x36,0x36,0x7F,0x36,0x7F,0x36,0x36,0x00},
    ['$'] = {0x0C,0x3E,0x03,0x1E,0x30,0x1F,0x0C,0x00},
    ['%'] = {0x00,0x63,0x33,0x18,0x0C,0x66,0x63,0x00},
    ['&'] = {0x1C,0x36,0x1C,0x6E,0x3B,0x33,0x6E,0x00},
    ['\'']= {0x06,0x06,0x03,0x00,0x00,0x00,0x00,0x00},
    ['('] = {0x18,0x0C,0x06,0x06,0x06,0x0C,0x18,0x00},
    [')'] = {0x06,0x0C,0x18,0x18,0x18,0x0C,0x06,0x00},
    ['*'] = {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00},
    ['+'] = {0x00,0x0C,0x0C,0x3F,0x0C,0x0C,0x00,0x00},
    [','] = {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x06},
    ['-'] = {0x00,0x00,0x00,0x3F,0x00,0x00,0x00,0x00},
    ['.'] = {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x00},
    ['/'] = {0x60,0x30,0x18,0x0C,0x06,0x03,0x01,0x00},
    ['0'] = {0x3E,0x63,0x73,0x7B,0x6F,0x67,0x3E,0x00},
    ['1'] = {0x0C,0x0E,0x0C,0x0C,0x0C,0x0C,0x3F,0x00},
    ['2'] = {0x1E,0x33,0x30,0x1C,0x06,0x33,0x3F,0x00},
    ['3'] = {0x1E,0x33,0x30,0x1C,0x30,0x33,0x1E,0x00},
    ['4'] = {0x38,0x3C,0x36,0x33,0x7F,0x30,0x78,0x00},
    ['5'] = {0x3F,0x03,0x1F,0x30,0x30,0x33,0x1E,0x00},
    ['6'] = {0x1C,0x06,0x03,0x1F,0x33,0x33,0x1E,0x00},
    ['7'] = {0x3F,0x33,0x30,0x18,0x0C,0x0C,0x0C,0x00},
    ['8'] = {0x1E,0x33,0x33,0x1E,0x33,0x33,0x1E,0x00},
    ['9'] = {0x1E,0x33,0x33,0x3E,0x30,0x18,0x0E,0x00},
    [':'] = {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x00},
    [';'] = {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x06},
    ['<'] = {0x18,0x0C,0x06,0x03,0x06,0x0C,0x18,0x00},
    ['='] = {0x00,0x00,0x3F,0x00,0x00,0x3F,0x00,0x00},
    ['>'] = {0x0C,0x18,0x30,0x60,0x30,0x18,0x0C,0x00},
    ['?'] = {0x1E,0x33,0x30,0x18,0x0C,0x00,0x0C,0x00},
    ['@'] = {0x3E,0x63,0x7B,0x7B,0x7B,0x03,0x1E,0x00},
    ['A'] = {0x0C,0x1E,0x33,0x33,0x3F,0x33,0x33,0x00},
    ['B'] = {0x3F,0x66,0x66,0x3E,0x66,0x66,0x3F,0x00},
    ['C'] = {0x3C,0x66,0x03,0x03,0x03,0x66,0x3C,0x00},
    ['D'] = {0x1F,0x36,0x66,0x66,0x66,0x36,0x1F,0x00},
    ['E'] = {0x7F,0x46,0x16,0x1E,0x16,0x46,0x7F,0x00},
    ['F'] = {0x7F,0x46,0x16,0x1E,0x16,0x06,0x0F,0x00},
    ['G'] = {0x3C,0x66,0x03,0x03,0x73,0x66,0x7C,0x00},
    ['H'] = {0x33,0x33,0x33,0x3F,0x33,0x33,0x33,0x00},
    ['I'] = {0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00},
    ['J'] = {0x78,0x30,0x30,0x30,0x33,0x33,0x1E,0x00},
    ['K'] = {0x67,0x66,0x36,0x1E,0x36,0x66,0x67,0x00},
    ['L'] = {0x0F,0x06,0x06,0x06,0x46,0x66,0x7F,0x00},
    ['M'] = {0x63,0x77,0x7F,0x7F,0x6B,0x63,0x63,0x00},
    ['N'] = {0x63,0x67,0x6F,0x7B,0x73,0x63,0x63,0x00},
    ['O'] = {0x1C,0x36,0x63,0x63,0x63,0x36,0x1C,0x00},
    ['P'] = {0x3F,0x66,0x66,0x3E,0x06,0x06,0x0F,0x00},
    ['Q'] = {0x1E,0x33,0x33,0x33,0x3B,0x1E,0x38,0x00},
    ['R'] = {0x3F,0x66,0x66,0x3E,0x36,0x66,0x67,0x00},
    ['S'] = {0x1E,0x33,0x07,0x0E,0x38,0x33,0x1E,0x00},
    ['T'] = {0x3F,0x2D,0x0C,0x0C,0x0C,0x0C,0x1E,0x00},
    ['U'] = {0x33,0x33,0x33,0x33,0x33,0x33,0x3F,0x00},
    ['V'] = {0x33,0x33,0x33,0x33,0x33,0x1E,0x0C,0x00},
    ['W'] = {0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00},
    ['X'] = {0x63,0x63,0x36,0x1C,0x1C,0x36,0x63,0x00},
    ['Y'] = {0x33,0x33,0x33,0x1E,0x0C,0x0C,0x1E,0x00},
    ['Z'] = {0x7F,0x63,0x31,0x18,0x4C,0x66,0x7F,0x00},
    ['['] = {0x1E,0x06,0x06,0x06,0x06,0x06,0x1E,0x00},
    ['\\']= {0x03,0x06,0x0C,0x18,0x30,0x60,0x40,0x00},
    [']'] = {0x1E,0x18,0x18,0x18,0x18,0x18,0x1E,0x00},
    ['^'] = {0x08,0x1C,0x36,0x63,0x00,0x00,0x00,0x00},
    ['_'] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF},
    ['`'] = {0x0C,0x0C,0x18,0x00,0x00,0x00,0x00,0x00},
    ['a'] = {0x00,0x00,0x1E,0x30,0x3E,0x33,0x6E,0x00},
    ['b'] = {0x07,0x06,0x06,0x3E,0x66,0x66,0x3B,0x00},
    ['c'] = {0x00,0x00,0x1E,0x33,0x03,0x33,0x1E,0x00},
    ['d'] = {0x38,0x30,0x30,0x3E,0x33,0x33,0x6E,0x00},
    ['e'] = {0x00,0x00,0x1E,0x33,0x3F,0x03,0x1E,0x00},
    ['f'] = {0x1C,0x36,0x06,0x0F,0x06,0x06,0x0F,0x00},
    ['g'] = {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x1F},
    ['h'] = {0x07,0x06,0x36,0x6E,0x66,0x66,0x67,0x00},
    ['i'] = {0x0C,0x00,0x0E,0x0C,0x0C,0x0C,0x1E,0x00},
    ['j'] = {0x30,0x00,0x30,0x30,0x30,0x33,0x33,0x1E},
    ['k'] = {0x07,0x06,0x66,0x36,0x1E,0x36,0x67,0x00},
    ['l'] = {0x0E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00},
    ['m'] = {0x00,0x00,0x33,0x7F,0x7F,0x6B,0x63,0x00},
    ['n'] = {0x00,0x00,0x1F,0x33,0x33,0x33,0x33,0x00},
    ['o'] = {0x00,0x00,0x1E,0x33,0x33,0x33,0x1E,0x00},
    ['p'] = {0x00,0x00,0x3B,0x66,0x66,0x3E,0x06,0x0F},
    ['q'] = {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x78},
    ['r'] = {0x00,0x00,0x3B,0x6E,0x66,0x06,0x0F,0x00},
    ['s'] = {0x00,0x00,0x3E,0x03,0x1E,0x30,0x1F,0x00},
    ['t'] = {0x08,0x0C,0x3E,0x0C,0x0C,0x2C,0x18,0x00},
    ['u'] = {0x00,0x00,0x33,0x33,0x33,0x33,0x6E,0x00},
    ['v'] = {0x00,0x00,0x33,0x33,0x33,0x1E,0x0C,0x00},
    ['w'] = {0x00,0x00,0x63,0x6B,0x7F,0x7F,0x36,0x00},
    ['x'] = {0x00,0x00,0x63,0x36,0x1C,0x36,0x63,0x00},
    ['y'] = {0x00,0x00,0x33,0x33,0x33,0x3E,0x30,0x1F},
    ['z'] = {0x00,0x00,0x3F,0x19,0x0C,0x26,0x3F,0x00},
    ['{'] = {0x38,0x0C,0x0C,0x07,0x0C,0x0C,0x38,0x00},
    ['|'] = {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00},
    ['}'] = {0x07,0x0C,0x0C,0x38,0x0C,0x0C,0x07,0x00},
    ['~'] = {0x6E,0x3B,0x00,0x00,0x00,0x00,0x00,0x00},
};

void vga_init(void) {
    vga_clear(0x0F);
    tx = 0; ty = 0;
    cur_attr = 0x0F;
    vga_set_cursor(0, 0);
}

void vga_clear(u8 attr) {
    volatile u16 *mem = VGA_TEXT_MEM;
    u16 v = (u16)((attr << 8) | ' ');
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) mem[i] = v;
}

static void vga_scroll_once(void) {
    for (int row = 1; row < VGA_HEIGHT; row++) {
        for (int col = 0; col < VGA_WIDTH; col++)
            VGA_TEXT_MEM[(row - 1) * VGA_WIDTH + col] =
                VGA_TEXT_MEM[row * VGA_WIDTH + col];
    }
    for (int col = 0; col < VGA_WIDTH; col++)
        VGA_TEXT_MEM[(VGA_HEIGHT - 1) * VGA_WIDTH + col] =
            (u16)((cur_attr << 8) | ' ');
}

void vga_scroll(void) {
    vga_scroll_once();
    ty = VGA_HEIGHT - 1;
    tx = 0;
    vga_set_cursor(tx, ty);
}

void vga_putc(char c, u8 attr) {
    cur_attr = attr;
    if (c == '\n') { ty++; tx = 0; }
    else if (c == '\r') { tx = 0; }
    else if (c == '\t') { tx = (tx + 4) & ~3; }
    else if (c == '\b') { if (tx > 0) tx--; }
    else {
        VGA_TEXT_MEM[ty * VGA_WIDTH + tx] = (u16)((attr << 8) | (u8)c);
        tx++;
    }
    if (tx >= VGA_WIDTH) { tx = 0; ty++; }
    if (ty >= VGA_HEIGHT) {
        vga_scroll_once();
        /* CRITICAL: clamp the row back into the screen.  Without this the
         * cursor keeps pointing past the last row and every following
         * character writes outside text memory - long output (help text!)
         * looked like the screen had been wiped. */
        ty = VGA_HEIGHT - 1;
    }
    vga_set_cursor(tx, ty);
}

void vga_write(const char *s, u8 attr) {
    while (*s) vga_putc(*s++, attr);
}

void vga_set_cursor(int x, int y) {
    u16 pos = (u16)(y * VGA_WIDTH + x);
    outb(0x3D4, 0x0F); outb(0x3D5, (u8)(pos & 0xFF));
    outb(0x3D4, 0x0E); outb(0x3D5, (u8)((pos >> 8) & 0xFF));
}

int vga_get_x(void) { return tx; }
int vga_get_y(void) { return ty; }

/* ------------------------------------------------------------------ */
/* graphics mode                                                      */
/* ------------------------------------------------------------------ */

/* Custom 16-colour palette (6-bit DAC values) in the classic EGA index
 * order, retuned for a modern dark-desktop look: charcoal window frames,
 * a deep blue desktop gradient and bright accent colours.  Only the DAC
 * values differ from stock EGA - every code in the kernel keeps its
 * semantic meaning (0x07 light content, 0x09 focused title bar, 0x0C
 * close box, ...). */
static const u8 vga_pal16[16][3] = {
    {  8, 10, 12},        /* 00 black  -> charcoal (shadows, taskbar)  */
    { 30, 36, 46},        /* 01 blue   -> desktop gradient top         */
    { 62, 74, 96},        /* 02 green  -> desktop gradient bottom      */
    { 20, 24, 30},        /* 03 cyan   -> window body (dark)           */
    {208, 52, 60},        /* 04 red    -> about title, errors          */
    { 86, 98,116},        /* 05 magenta-> window body (light)          */
    {128,138,152},        /* 06 brown  -> secondary text               */
    {202,208,216},        /* 07 grey   -> normal text                  */
    { 56, 62, 74},        /* 08 d.grey -> unfocused title bar          */
    { 46,110,210},        /* 09 d.blue -> focused title bar            */
    { 70,190, 90},        /* 10 green  -> ok / values                  */
    { 60,140,230},        /* 11 cyan   -> bright accent                */
    {230, 60, 50},        /* 12 red    -> close box                    */
    {245,180, 40},        /* 13 yellow -> hints                        */
    {120,190,250},        /* 14 yellow -> hints (light blue)           */
    {245,247,250}         /* 15 white  -> titles, borders              */
};

/* Desktop gradient: the 240 palette slots above index 15 normally repeat
 * the base colours; instead fill them with a smooth dark-blue ramp so the
 * desktop background can fade from top to bottom instead of being one
 * flat colour.  Indices 16..63 ramp between palette 1 and 2, 64..255 are
 * kept as safe repeats of the base colours.
 *
 * The table above holds 8-bit colour values, but the VGA DAC registers
 * are 6 bits wide: every byte going to port 0x3C9 must be reduced with
 * >>2 first.  Writing the raw 8-bit values made the emulated DAC scale
 * them by 4 with 8-bit wraparound, turning anything above 63 into the
 * wrong hue (the "rainbow desktop"). */
static void vga_load_palette(void) {
    outb(0x3C8, 0);                 /* start at DAC index 0 */
    for (int i = 0; i < 16; i++) {
        outb(0x3C9, vga_pal16[i][0] >> 2);
        outb(0x3C9, vga_pal16[i][1] >> 2);
        outb(0x3C9, vga_pal16[i][2] >> 2);
    }
    /* 48-step gradient ramp: index 16 + k, k = 0..47 */
    for (int k = 0; k < 48; k++) {
        u32 a = 47 - k, b = k;      /* weight: starts at colour 1, ends 2 */
        outb(0x3C9, (u8)(((vga_pal16[1][0] >> 2) * a + (vga_pal16[2][0] >> 2) * b) / 47));
        outb(0x3C9, (u8)(((vga_pal16[1][1] >> 2) * a + (vga_pal16[2][1] >> 2) * b) / 47));
        outb(0x3C9, (u8)(((vga_pal16[1][2] >> 2) * a + (vga_pal16[2][2] >> 2) * b) / 47));
    }
    for (int i = 64; i < 256; i++) {
        const u8 *c = vga_pal16[i & 15];
        outb(0x3C9, c[0] >> 2);
        outb(0x3C9, c[1] >> 2);
        outb(0x3C9, c[2] >> 2);
    }
}

void vga_set_mode13h(void) {
    /* The old sequence only poked a handful of registers with values that
     * do not belong to mode 13h at all (CR06 = 0x0B makes the frame 11
     * scanlines tall) and never programmed the attribute controller, so
     * the hardware scanned out streaked garbage.  What follows is the full
     * canonical IBM VGA mode-13h mode set: sequencer -> CRTC -> graphics
     * controller -> attribute controller -> DAC palette. */

    /* ---- sequencer: reset, reconfigure, release ---------------------- */
    outb(0x3C4, 0x00); outb(0x3C5, 0x03);   /* synchronous reset */
    outb(0x3C4, 0x01); outb(0x3C5, 0x01);   /* 8-dot char clock */
    outb(0x3C4, 0x02); outb(0x3C5, 0x0F);   /* write plane mask: all planes */
    outb(0x3C4, 0x03); outb(0x3C5, 0x00);   /* character map select */
    outb(0x3C4, 0x04); outb(0x3C5, 0x0E);   /* memory mode: chain-4 + ext */
    outb(0x3C4, 0x00); outb(0x3C5, 0x03);   /* leave reset */

    /* ---- misc output: 25 MHz dot clock, CRTC at 0x3D4 ---------------- */
    outb(0x3C2, 0x63);

    /* ---- CRTC: standard mode-13h timings (200 lines, double-scanned) -- */
    static const u8 crtc[25] = {
        0x5F, 0x4F, 0x50, 0x82, 0x54, 0x80,          /* horizontal */
        0xBF, 0x1F, 0x00, 0x41, 0x00, 0x00,          /* vertical   */
        0x00, 0x00, 0x00, 0x00,                      /* addresses  */
        0x9C, 0x8E, 0x8F, 0x28,                      /* retrace/offset */
        0x40, 0x96, 0xB9, 0xA3, 0xFF
    };
    outb(0x3D4, 0x11); outb(0x3D5, 0x00);   /* unprotect CR00-CR07 */
    for (int i = 0; i < 25; i++) {
        outb(0x3D4, (u8)i);
        outb(0x3D5, crtc[i]);
    }

    /* ---- graphics controller ------------------------------------------ */
    static const u8 gdc[9] = { 0x00, 0x00, 0x00, 0x00, 0x00,
                               0x40, 0x05, 0x0F, 0xFF };
    for (int i = 0; i < 9; i++) {
        outb(0x3CE, (u8)i);
        outb(0x3CF, gdc[i]);
    }

    /* ---- attribute controller: graphics mode + identity palette ------- */
    (void)inb(0x3DA);                       /* reset the index/data FF */
    for (int i = 0; i < 16; i++) {
        outb(0x3C0, (u8)i);                 /* AC00-AC0F */
        outb(0x3C0, (u8)i);                 /* identity palette */
    }
    outb(0x3C0, 0x10); outb(0x3C0, 0x41);   /* graphics mode, 8-bit pixels */
    outb(0x3C0, 0x11); outb(0x3C0, 0x00);   /* overscan = black */
    outb(0x3C0, 0x20);                      /* release video passthrough */

    outb(0x3C6, 0xFF);                      /* pixel mask */
    vga_load_palette();
    gfx_clear(0x00);
}

void gfx_putpixel(int x, int y, u8 color) {
    if (x < 0 || x >= GFX_W || y < 0 || y >= GFX_H) return;
    VGA_FB[y * GFX_W + x] = color;
}

void gfx_fillrect(int x0, int y0, int x1, int y1, u8 color) {
    /* clip once, then memset each row: the old per-pixel loop re-did the
     * bounds test 64000 times per repaint and dominated the drag latency */
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= GFX_W) x1 = GFX_W - 1;
    if (y1 >= GFX_H) y1 = GFX_H - 1;
    if (x0 > x1 || y0 > y1) return;
    u32 w = (u32)(x1 - x0 + 1);
    for (int y = y0; y <= y1; y++)
        memset((void*)(VGA_FB + y * GFX_W + x0), color, w);
}

void gfx_drawchar(int x, int y, char c, u8 fg, u8 bg) {
    const u8 *glyph = font8x8[(u8)c & 0x7F];
    for (int row = 0; row < 8; row++) {
        for (int col = 0; col < 8; col++)
            /* the font table is LSB-left: bit 0 of each byte is the
             * leftmost pixel (mirrored glyphs otherwise) */
            gfx_putpixel(x + col, y + row,
                         (glyph[row] & (1u << col)) ? fg : bg);
    }
}

void gfx_drawstring(int x, int y, const char *s, u8 fg, u8 bg) {
    while (*s) {
        gfx_drawchar(x, y, *s++, fg, bg);
        x += 8;
    }
}

void gfx_clear(u8 color) {
    memset((void*)VGA_FB, color, GFX_W * GFX_H);
}

/* ================================================================== */
/* true colour framebuffer (VBE linear framebuffer)                    */
/*                                                                    */
/* The rest of the kernel only ever needed 256 palette indices, which  */
/* is what forced the old 320x200 mode.  The desktop now draws in      */
/* real 32bpp colour on whatever mode the loader programmed, so this   */
/* section adds a small immediate-mode API on top of the same font.    */
/* ================================================================== */
#include "vbe.h"

static u32 *fb32 = 0;
static int  fbw = 0, fbh = 0, fbp = 0;      /* fbp: stride in pixels */

int gfx_hw_init(void) {
    if (!vbe_init()) { fb32 = 0; return 0; }
    fb32 = vbe_fb();
    fbw  = vbe_width();
    fbh  = vbe_height();
    fbp  = vbe_pitch_px();
    if (!fb32 || fbw <= 0 || fbh <= 0) { fb32 = 0; return 0; }
    return 1;
}

int gfx_is_vbe(void)    { return fb32 != 0; }
int gfx_screen_w(void)  { return fb32 ? fbw : GFX_W; }
int gfx_screen_h(void)  { return fb32 ? fbh : GFX_H; }

void gfx_px(int x, int y, u32 rgb) {
    if (!fb32) return;
    if (x < 0 || x >= fbw || y < 0 || y >= fbh) return;
    fb32[(u32)y * (u32)fbp + (u32)x] = rgb;
}

static u32 blend(u32 dst, u32 src, int a) {
    u32 rb = dst & 0x00FF00FFu, g = dst & 0x0000FF00u;
    u32 sr = src & 0x00FF00FFu, sg = src & 0x0000FF00u;
    rb = (((sr - rb) * (u32)a) >> 8) + rb;
    g  = (((sg - g)  * (u32)a) >> 8) + g;
    return (rb & 0x00FF00FFu) | (g & 0x0000FF00u);
}

void gfx_rect(int x0, int y0, int x1, int y1, u32 rgb) {
    if (!fb32) return;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= fbw) x1 = fbw - 1;
    if (y1 >= fbh) y1 = fbh - 1;
    if (x0 > x1 || y0 > y1) return;
    for (int y = y0; y <= y1; y++) {
        u32 *row = fb32 + (u32)y * (u32)fbp;
        for (int x = x0; x <= x1; x++) row[x] = rgb;
    }
}

void gfx_rect_a(int x0, int y0, int x1, int y1, u32 rgb, int a) {
    if (!fb32) return;
    if (a <= 0) return;
    if (a >= 255) { gfx_rect(x0, y0, x1, y1, rgb); return; }
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= fbw) x1 = fbw - 1;
    if (y1 >= fbh) y1 = fbh - 1;
    if (x0 > x1 || y0 > y1) return;
    for (int y = y0; y <= y1; y++) {
        u32 *row = fb32 + (u32)y * (u32)fbp;
        for (int x = x0; x <= x1; x++) row[x] = blend(row[x], rgb, a);
    }
}

/* horizontal run helper: fills [x0,x1] on row y only for the rounded
 * span of that row, which keeps the corner cost proportional to r */
static int isqrt_i(int v) {
    int r = 0;
    while ((r + 1) * (r + 1) <= v) r++;
    return r;
}

static void round_rows(int x0, int y0, int x1, int y1, int r,
                       u32 rgb, int a, int ring_only) {
    if (!fb32) return;
    if (r < 0) r = 0;
    int maxr = (x1 - x0) / 2;
    if ((y1 - y0) / 2 < maxr) maxr = (y1 - y0) / 2;
    if (r > maxr) r = maxr;

    for (int y = y0; y <= y1; y++) {
        int inset = 0;
        int dy = y - y0;
        if (dy < r)                inset = r - isqrt_i(r * r - (r - dy) * (r - dy));
        else if (y1 - y < r)       { int d = y1 - y; inset = r - isqrt_i(r * r - (r - d) * (r - d)); }
        int lx = x0 + inset, rx = x1 - inset;
        if (lx > rx) continue;

        if (ring_only) {
            /* top / bottom rows of the ring and its vertical sides */
            int edge_row = (y < y0 + 1) || (y > y1 - 1);
            if (edge_row) {
                if (a >= 255) { for (int x = lx; x <= rx; x++) gfx_px(x, y, rgb); }
                else          { for (int x = lx; x <= rx; x++) {
                                    if (x >= 0 && x < fbw && y >= 0 && y < fbh)
                                        fb32[(u32)y * fbp + x] = blend(fb32[(u32)y * fbp + x], rgb, a); } }
            } else {
                gfx_px(lx, y, rgb);
                gfx_px(rx, y, rgb);
            }
        } else {
            if (a >= 255) gfx_rect(lx, y, rx, y, rgb);
            else          gfx_rect_a(lx, y, rx, y, rgb, a);
        }
    }
}

void gfx_round(int x0, int y0, int x1, int y1, int r, u32 rgb) {
    round_rows(x0, y0, x1, y1, r, rgb, 255, 0);
}
void gfx_round_a(int x0, int y0, int x1, int y1, int r, u32 rgb, int a) {
    round_rows(x0, y0, x1, y1, r, rgb, a, 0);
}
void gfx_ring(int x0, int y0, int x1, int y1, int r, u32 rgb) {
    round_rows(x0, y0, x1, y1, r, rgb, 255, 1);
}

void gfx_grad(int x0, int y0, int x1, int y1, u32 top, u32 bot) {
    if (!fb32) return;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= fbw) x1 = fbw - 1;
    if (y1 >= fbh) y1 = fbh - 1;
    if (x0 > x1 || y0 > y1) return;
    int h = y1 - y0;
    for (int y = y0; y <= y1; y++) {
        int t = h ? (y - y0) * 255 / h : 0;
        u32 c = blend(top, bot, t);
        u32 *row = fb32 + (u32)y * (u32)fbp;
        for (int x = x0; x <= x1; x++) row[x] = c;
    }
}

void gfx_char(int x, int y, char c, u32 fg, int scale) {
    if (!fb32) return;
    if (scale < 1) scale = 1;
    const u8 *glyph = font8x8[(u8)c & 0x7F];
    for (int row = 0; row < 8; row++) {
        u8 bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            if (!(bits & (1u << col))) continue;
            int px = x + col * scale, py = y + row * scale;
            if (scale == 1) { gfx_px(px, py, fg); continue; }
            gfx_rect(px, py, px + scale - 1, py + scale - 1, fg);
        }
    }
}

void gfx_str(int x, int y, const char *s, u32 fg, int scale) {
    if (scale < 1) scale = 1;
    while (*s) {
        gfx_char(x, y, *s++, fg, scale);
        x += 8 * scale;
    }
}

int gfx_strw(const char *s, int scale) {
    if (scale < 1) scale = 1;
    int n = 0;
    while (s[n]) n++;
    return n * 8 * scale;
}

int gfx_strh(int scale) { return 8 * (scale < 1 ? 1 : scale); }
