/* NovaOS - GUI desktop */
#include "gui.h"
#include "vga.h"
#include "keyboard.h"
#include "mouse.h"
#include "string.h"
#include "printf.h"
#include "serial.h"

#define MAX_WIN   8
#define TASKBAR_H 12

enum { WIN_CONSOLE, WIN_ABOUT, WIN_MEMINFO };

typedef struct {
    int  used;
    int  type;
    int  x, y, w, h;
    int  focused;
    char title[24];
} window_t;

static window_t wins[MAX_WIN];
static int focus = -1;

static const char *win_title(int type) {
    switch (type) {
    case WIN_CONSOLE: return "Console";
    case WIN_ABOUT:   return "About NovaOS";
    case WIN_MEMINFO: return "Memory Info";
    }
    return "Window";
}

static void win_draw_frame(window_t *w) {
    u8 title_bg = w->focused ? 0x01 : 0x08;
    gfx_fillrect(w->x, w->y, w->x + w->w - 1, w->y + 11, title_bg);
    gfx_fillrect(w->x, w->y + 12, w->x + w->w - 1, w->y + w->h - 1, 0x07);
    /* border */
    gfx_fillrect(w->x, w->y, w->x + w->w - 1, w->y, 0x0F);
    gfx_fillrect(w->x, w->y + w->h - 1, w->x + w->w - 1, w->y + w->h - 1, 0x0F);
    gfx_fillrect(w->x, w->y, w->x, w->y + w->h - 1, 0x0F);
    gfx_fillrect(w->x + w->w - 1, w->y, w->x + w->w - 1, w->y + w->h - 1, 0x0F);
    gfx_drawstring(w->x + 3, w->y + 2, w->title, 0x0F, title_bg);
}

static void win_draw_content(window_t *w) {
    switch (w->type) {
    case WIN_ABOUT:
        gfx_drawstring(w->x + 4, w->y + 16, "NovaOS v0.5", 0x04, 0x07);
        gfx_drawstring(w->x + 4, w->y + 26, "Self-made kernel GUI", 0x01, 0x07);
        gfx_drawstring(w->x + 4, w->y + 36, "GDT/IDT/paging/mm", 0x01, 0x07);
        gfx_drawstring(w->x + 4, w->y + 46, "ring3/FAT12/mouse", 0x01, 0x07);
        gfx_drawstring(w->x + 4, w->y + 56, "mouse: drag title bar", 0x0E, 0x07);
        break;
    case WIN_MEMINFO: {
        extern u32 mm_free_frames(void);
        extern u32 kheap_used(void);
        char buf[48];
        /* frame count -> KB */
        u32 free_kb = mm_free_frames() * 4;
        gfx_drawstring(w->x + 4, w->y + 16, "phys free: ", 0x01, 0x07);
        ksprintf(buf, "%u KB", free_kb);
        gfx_drawstring(w->x + 60, w->y + 16, buf, 0x04, 0x07);
        gfx_drawstring(w->x + 4, w->y + 28, "heap used: ", 0x01, 0x07);
        ksprintf(buf, "%u B", kheap_used());
        gfx_drawstring(w->x + 60, w->y + 28, buf, 0x04, 0x07);
        break;
    }
    case WIN_CONSOLE:
    default:
        gfx_drawstring(w->x + 4, w->y + 16, "kernel: hello from C", 0x02, 0x07);
        gfx_drawstring(w->x + 4, w->y + 26, "boot: loader->pmode", 0x02, 0x07);
        gfx_drawstring(w->x + 4, w->y + 36, "user: ring3 process", 0x02, 0x07);
        break;
    }
}

static void redraw_all(void) {
    gfx_clear(0x03);   /* desktop background */
    for (int i = 0; i < MAX_WIN; i++) {
        if (!wins[i].used) continue;
        win_draw_frame(&wins[i]);
        win_draw_content(&wins[i]);
    }
    /* taskbar */
    gfx_fillrect(0, GFX_H - TASKBAR_H, GFX_W - 1, GFX_H - 1, 0x00);
    gfx_drawstring(4, GFX_H - 10, "NovaOS Desktop", 0x0A, 0x00);
    for (int i = 0; i < MAX_WIN; i++) {
        if (!wins[i].used) continue;
        gfx_drawstring(120 + i * 48, GFX_H - 10, wins[i].title, 0x0F, 0x00);
    }
}

static int win_open(int type) {
    for (int i = 0; i < MAX_WIN; i++) {
        if (wins[i].used) continue;
        window_t *w = &wins[i];
        w->used = 1;
        w->type = type;
        w->x = 12 + (i % 3) * 28;
        w->y = 14 + (i % 3) * 22;
        w->w = 150;
        w->h = 84;
        w->focused = 1;
        strncpy(w->title, win_title(type), sizeof(w->title) - 1);
        w->title[sizeof(w->title) - 1] = '\0';
        if (focus >= 0 && wins[focus].used) wins[focus].focused = 0;
        focus = i;
        redraw_all();
        return i;
    }
    return -1;
}

static void win_close(int idx) {
    if (idx < 0 || idx >= MAX_WIN || !wins[idx].used) return;
    wins[idx].used = 0;
    focus = -1;
    for (int i = MAX_WIN - 1; i >= 0; i--)
        if (wins[i].used) { focus = i; break; }
    if (focus >= 0) wins[focus].focused = 1;
    redraw_all();
}

/* window whose title bar contains (x,y), or -1 */
static int win_at_title(int x, int y) {
    for (int i = MAX_WIN - 1; i >= 0; i--) {
        if (!wins[i].used) continue;
        if (x >= wins[i].x && x < wins[i].x + wins[i].w &&
            y >= wins[i].y && y <= wins[i].y + 11)
            return i;
    }
    return -1;
}

static void draw_cursor(void) {
    int x = mouse_pos_x(), y = mouse_pos_y();
    gfx_putpixel(x, y, 0x0F);
    for (int i = 1; i <= 4; i++) {
        if (x - i >= 0)      gfx_putpixel(x - i, y, 0x0F);
        if (x + i < GFX_W)   gfx_putpixel(x + i, y, 0x0F);
        if (y - i >= 0)      gfx_putpixel(x, y - i, 0x0F);
        if (y + i < GFX_H)   gfx_putpixel(x, y + i, 0x0F);
    }
}

void gui_enter(void) {
    static int drag = -1, drag_ox = 0, drag_oy = 0;

    vga_set_mode13h();
    for (int i = 0; i < MAX_WIN; i++) wins[i].used = 0;
    focus = -1;
    win_open(WIN_CONSOLE);
    win_open(WIN_ABOUT);

    for (;;) {
        /* --- mouse: click title bar -> focus + drag ------------------- */
        if (mouse_left_clicked()) {
            int w = win_at_title(mouse_pos_x(), mouse_pos_y());
            if (w >= 0) {
                if (focus >= 0) wins[focus].focused = 0;
                focus = w;
                wins[w].focused = 1;
                drag = w;
                drag_ox = mouse_pos_x() - wins[w].x;
                drag_oy = mouse_pos_y() - wins[w].y;
                redraw_all();
            }
        }
        if (mouse_left_released()) {
            drag = -1;
        }
        if (drag >= 0 && mouse_left_down()) {
            int nx = mouse_pos_x() - drag_ox;
            int ny = mouse_pos_y() - drag_oy;
            if (nx < 0) nx = 0;
            if (ny < 0) ny = 0;
            if (nx + wins[drag].w > GFX_W) nx = GFX_W - wins[drag].w;
            if (ny + wins[drag].h > GFX_H - 12) ny = GFX_H - 12 - wins[drag].h;
            if (nx != wins[drag].x || ny != wins[drag].y) {
                wins[drag].x = nx;
                wins[drag].y = ny;
                redraw_all();
            }
        }

        /* --- keyboard ------------------------------------------------- */
        int c = keyboard_getc();
        if (c) {
            if (c == 27) {                        /* Esc: close focused */
                if (focus >= 0) win_close(focus);
            } else switch (c) {
            case 'c': case 'C': win_open(WIN_CONSOLE); break;
            case 'a': case 'A': win_open(WIN_ABOUT); break;
            case 'm': case 'M': win_open(WIN_MEMINFO); break;
            case '\t': {                          /* cycle focus */
                if (focus >= 0) wins[focus].focused = 0;
                do { focus = (focus + 1) % MAX_WIN; } while (!wins[focus].used);
                wins[focus].focused = 1;
                redraw_all();
                break;
            }
            default: break;
            }
        }

        draw_cursor();
    }
}
