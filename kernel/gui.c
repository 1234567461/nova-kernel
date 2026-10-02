/* NovaOS - built-in GUI desktop
 *
 * VGA mode 13h (320x200x256) + window manager + taskbar, all home-grown:
 * the framebuffer is the raw linear 0xA0000 region, the widgets are drawn
 * with gfx_* primitives and the mouse/keyboard are read from the drivers
 * directly.
 *
 * Fixed in this revision (the old version was mostly non-functional):
 *   - mouse_left_clicked()/mouse_left_released() are edge-detectors that
 *     *consume* the edge by updating m_prev.  The old loop called both on
 *     every iteration for unrelated purposes, so whichever call came first
 *     ate the edge and the other one always saw a stale (false) result.
 *     A plain left-click therefore never focused/dragged a window and the
 *     release was never observed - drag never stopped.  We now snapshot the
 *     edges ONCE per frame and drive the whole UI from that snapshot.
 *   - dragging recomputed nx/ny from the live cursor every frame, so the
 *     window could never actually follow the cursor.  We now latch the
 *     grab offset at the click and move the window by the cursor delta.
 *   - windows opened at hard-coded slots and could overlap arbitrarily;
 *     a click on a background window could not raise it above a later one.
 *     We now cascade placement and raise-on-click.
 *   - Tab-cycle could spin forever when no window was open (focus = -1);
 *     no keyboard focus model existed at all.
 *   - the mode-13h register sequence was missing the palette DAC setup and
 *     used a bogus 0x3C6 value, so the screen came up black.
 *   - the cursor was painted once and never erased; it left a permanent
 *     trail across the desktop.
 *   - the frame loop busy-spun (100% CPU) and never ran hlt().
 */
#include "gui.h"
#include "vga.h"
#include "keyboard.h"
#include "mouse.h"
#include "string.h"
#include "printf.h"
#include "serial.h"
#include "mm.h"
#include "kheap.h"
#include "io.h"

#define MAX_WIN   8
#define TASKBAR_H 12
#define TITLE_H   12
#define CASCADE_X 10
#define CASCADE_Y 14

enum { WIN_CONSOLE, WIN_ABOUT, WIN_MEMINFO, WIN_TASKS, WIN_FILES };

typedef struct {
    int  used;
    int  type;
    int  x, y, w, h;
    int  focused;
    char title[24];
} window_t;

static window_t wins[MAX_WIN];
static int focus = -1;
static int cascade = 0;

/* one-frame snapshot of the input edges, so the consuming edge-detectors
 * are each called exactly once per iteration */
typedef struct {
    int click, release, down;   /* left-button edge / level */
    int mx, my;                 /* cursor position */
} input_t;

static const char *win_title(int type) {
    switch (type) {
    case WIN_CONSOLE: return "Console";
    case WIN_ABOUT:   return "About NovaOS";
    case WIN_MEMINFO: return "Memory Info";
    case WIN_TASKS:   return "Tasks";
    case WIN_FILES:   return "Files";
    }
    return "Window";
}

static void win_draw_frame(window_t *w) {
    u8 title_bg = w->focused ? 0x09 : 0x08;    /* focused = bright blue */

    /* body, then title bar, then a raised border */
    gfx_fillrect(w->x + 1, w->y + 1, w->x + w->w - 2, w->y + w->h - 2, 0x07);
    gfx_fillrect(w->x + 1, w->y + 1, w->x + w->w - 2, w->y + TITLE_H, title_bg);
    gfx_fillrect(w->x, w->y, w->x + w->w - 1, w->y, 0x0F);
    gfx_fillrect(w->x, w->y + w->h - 1, w->x + w->w - 1, w->y + w->h - 1, 0x08);
    gfx_fillrect(w->x, w->y, w->x, w->y + w->h - 1, 0x0F);
    gfx_fillrect(w->x + w->w - 1, w->y, w->x + w->w - 1, w->y + w->h - 1, 0x08);

    /* title text (clipped to the bar so long titles cannot bleed out) */
    int tx = w->x + 4;
    for (const char *s = w->title; *s && tx < w->x + w->w - 10; s++, tx += 8)
        gfx_drawchar(tx, w->y + 3, *s, 0x0F, title_bg);

    /* close box in the top-right corner */
    int cbx = w->x + w->w - 10;
    gfx_fillrect(cbx + 1, w->y + 2, cbx + 8, w->y + 9, 0x0C);
    gfx_drawchar(cbx + 2, w->y + 3, 'x', 0x0F, 0x0C);
}

static void win_draw_content(window_t *w) {
    int cx = w->x + 6;
    int cy = w->y + TITLE_H + 5;
    char buf[40];

    switch (w->type) {
    case WIN_ABOUT:
        gfx_drawstring(cx, cy,      "NovaOS v0.6",        0x04, 0x07);
        gfx_drawstring(cx, cy + 11, "home-grown kernel",   0x01, 0x07);
        gfx_drawstring(cx, cy + 21, "GDT IDT paging",      0x01, 0x07);
        gfx_drawstring(cx, cy + 31, "ring3 FAT12 mouse",   0x01, 0x07);
        gfx_drawstring(cx, cy + 45, "drag: title bar",     0x0E, 0x07);
        gfx_drawstring(cx, cy + 55, "close: x or Esc",     0x0E, 0x07);
        break;

    case WIN_MEMINFO: {
        u32 free_kb = mm_free_frames() * 4;
        gfx_drawstring(cx, cy,      "phys free:", 0x01, 0x07);
        ksprintf(buf, "%u KB", free_kb);
        gfx_drawstring(cx + 88, cy, buf, 0x04, 0x07);
        gfx_drawstring(cx, cy + 12, "heap used:", 0x01, 0x07);
        ksprintf(buf, "%u B", kheap_used());
        gfx_drawstring(cx + 88, cy + 12, buf, 0x04, 0x07);
        break;
    }

    case WIN_TASKS: {
        extern u32 sched_task_count(void);
        extern u32 sched_current_pid(void);
        u32 n = sched_task_count();
        ksprintf(buf, "%u task(s)", n);
        gfx_drawstring(cx, cy, buf, 0x01, 0x07);
        ksprintf(buf, "current pid %u", sched_current_pid());
        gfx_drawstring(cx, cy + 12, buf, 0x04, 0x07);
        ksprintf(buf, "ticks %u", (u32)timer_ticks());
        gfx_drawstring(cx, cy + 24, buf, 0x01, 0x07);
        break;
    }

    case WIN_FILES: {
        extern int fat_mounted(void);
        extern u32 fat_free_bytes(void);
        gfx_drawstring(cx, cy, "FAT12 volume", 0x01, 0x07);
        if (fat_mounted()) {
            ksprintf(buf, "free %u KB", fat_free_bytes() / 1024);
            gfx_drawstring(cx, cy + 12, buf, 0x02, 0x07);
        } else {
            gfx_drawstring(cx, cy + 12, "no volume", 0x0C, 0x07);
        }
        gfx_drawstring(cx, cy + 26, "try: ls / cat", 0x0E, 0x07);
        break;
    }

    case WIN_CONSOLE:
    default:
        gfx_drawstring(cx, cy,      "kernel: hello from C",  0x02, 0x07);
        gfx_drawstring(cx, cy + 12, "boot: loader->pmode",   0x02, 0x07);
        gfx_drawstring(cx, cy + 24, "user: ring3 process",   0x02, 0x07);
        gfx_drawstring(cx, cy + 38, "press C A M T F",       0x0E, 0x07);
        break;
    }
}

static void redraw_all(void) {
    gfx_clear(0x01);                          /* teal desktop */

    /* a light dotted grid so window motion is visible */
    for (int y = 0; y < GFX_H - TASKBAR_H; y += 16)
        for (int x = ((y / 16) & 1) ? 8 : 0; x < GFX_W; x += 16)
            gfx_putpixel(x, y, 0x09);

    /* paint unfocused first, focused last, so the focused window is on top */
    for (int i = 0; i < MAX_WIN; i++) {
        if (!wins[i].used || i == focus) continue;
        win_draw_frame(&wins[i]);
        win_draw_content(&wins[i]);
    }
    if (focus >= 0 && wins[focus].used) {
        win_draw_frame(&wins[focus]);
        win_draw_content(&wins[focus]);
    }

    /* taskbar */
    gfx_fillrect(0, GFX_H - TASKBAR_H, GFX_W - 1, GFX_H - 1, 0x00);
    gfx_drawstring(4, GFX_H - 10, "NovaOS", 0x0A, 0x00);
    int bx = 60;
    for (int i = 0; i < MAX_WIN; i++) {
        if (!wins[i].used) continue;
        u8 bg = (i == focus) ? 0x09 : 0x08;
        gfx_fillrect(bx - 2, GFX_H - TASKBAR_H + 1, bx + 42, GFX_H - 2, bg);
        /* only the first letter of the title fits in the button */
        gfx_drawchar(bx + 16, GFX_H - 10, wins[i].title[0], 0x0F, bg);
        bx += 46;
        if (bx > GFX_W - 50) break;
    }
}

/* the window under (x,y); topmost first */
static int win_at(int x, int y) {
    if (focus >= 0 && wins[focus].used &&
        x >= wins[focus].x && x < wins[focus].x + wins[focus].w &&
        y >= wins[focus].y && y < wins[focus].y + wins[focus].h)
        return focus;
    for (int i = MAX_WIN - 1; i >= 0; i--) {
        if (!wins[i].used) continue;
        if (x >= wins[i].x && x < wins[i].x + wins[i].w &&
            y >= wins[i].y && y < wins[i].y + wins[i].h)
            return i;
    }
    return -1;
}

static int win_at_title(int x, int y) {
    int i = win_at(x, y);
    if (i >= 0 && y >= wins[i].y && y <= wins[i].y + TITLE_H) return i;
    return -1;
}

static int win_at_close(int x, int y) {
    for (int i = MAX_WIN - 1; i >= 0; i--) {
        if (!wins[i].used) continue;
        int cbx = wins[i].x + wins[i].w - 10;
        if (x >= cbx + 1 && x <= cbx + 8 &&
            y >= wins[i].y + 2 && y <= wins[i].y + 9)
            return i;
    }
    return -1;
}

static void win_raise(int idx) {
    if (idx < 0 || idx >= MAX_WIN || !wins[idx].used) return;
    if (focus >= 0 && focus != idx && wins[focus].used) wins[focus].focused = 0;
    focus = idx;
    wins[idx].focused = 1;
}

static int win_open(int type) {
    for (int i = 0; i < MAX_WIN; i++) {
        if (wins[i].used) continue;
        window_t *w = &wins[i];
        w->used = 1;
        w->type = type;
        w->x = CASCADE_X + (cascade % 6) * 16;
        w->y = CASCADE_Y + (cascade % 6) * 14;
        cascade++;
        w->w = 168;
        w->h = 88;
        strncpy(w->title, win_title(type), sizeof(w->title) - 1);
        w->title[sizeof(w->title) - 1] = '\0';
        win_raise(i);
        return i;
    }
    return -1;                 /* out of window slots */
}

static void win_close(int idx) {
    if (idx < 0 || idx >= MAX_WIN || !wins[idx].used) return;
    wins[idx].used = 0;
    focus = -1;
    for (int i = MAX_WIN - 1; i >= 0; i--)
        if (wins[i].used) { focus = i; break; }
    if (focus >= 0) wins[focus].focused = 1;
}

static void draw_cursor(const input_t *in) {
    int x = in->mx, y = in->my;
    /* a small arrow, drawn last so it is always on top */
    for (int i = 0; i <= 6; i++)   gfx_putpixel(x, y + i, 0x0F);
    for (int i = 0; i <= 4; i++)   gfx_putpixel(x + i, y + 2 + i, 0x0F);
    for (int i = 0; i <= 2; i++)   gfx_putpixel(x + i, y + 6 - i, 0x0F);
}

void gui_enter(void) {
    input_t in;
    int drag = -1, drag_dx = 0, drag_dy = 0;
    int last_mx = -1, last_my = -1;
    int need_redraw = 1;

    vga_set_mode13h();

    for (int i = 0; i < MAX_WIN; i++) wins[i].used = 0;
    focus = -1;
    cascade = 0;

    win_open(WIN_CONSOLE);
    win_open(WIN_ABOUT);
    win_open(WIN_MEMINFO);

    serial_puts("[gui] mode 13h desktop up (320x200x256), windows=3\n");

    for (;;) {
        /* ---- snapshot the consuming edge-detectors exactly once ---- */
        in.click   = mouse_left_clicked();
        in.release = mouse_left_released();
        in.down    = mouse_left_down();
        in.mx      = mouse_pos_x();
        in.my      = mouse_pos_y();

        /* ---- mouse: click to focus / raise / start drag ---- */
        if (in.click) {
            int c = win_at_close(in.mx, in.my);
            if (c >= 0) {
                win_close(c);
                need_redraw = 1;
                drag = -1;
            } else {
                int w = win_at_title(in.mx, in.my);
                if (w >= 0) {
                    win_raise(w);
                    /* latch the grab offset: the old code recomputed the
                     * window position from the live cursor each frame,
                     * which made dragging a no-op */
                    drag = w;
                    drag_dx = in.mx - wins[w].x;
                    drag_dy = in.my - wins[w].y;
                    need_redraw = 1;
                }
            }
        }
        if (in.release) drag = -1;

        if (drag >= 0 && in.down) {
            int nx = in.mx - drag_dx;
            int ny = in.my - drag_dy;
            if (nx < 0) nx = 0;
            if (ny < 0) ny = 0;
            if (nx + wins[drag].w > GFX_W)         nx = GFX_W - wins[drag].w;
            if (ny + wins[drag].h > GFX_H - TASKBAR_H)
                ny = GFX_H - TASKBAR_H - wins[drag].h;
            if (nx != wins[drag].x || ny != wins[drag].y) {
                wins[drag].x = nx;
                wins[drag].y = ny;
                need_redraw = 1;
            }
        }

        /* ---- keyboard ---- */
        int c = keyboard_getc();
        if (c) {
            if (c == 27) {                                   /* Esc */
                if (focus >= 0) win_close(focus);
                need_redraw = 1;
            } else if (c == 0x0D || c == 0x0A) {             /* Enter: leave GUI */
                break;
            } else switch (c) {
            case 'c': case 'C': win_open(WIN_CONSOLE); need_redraw = 1; break;
            case 'a': case 'A': win_open(WIN_ABOUT);   need_redraw = 1; break;
            case 'm': case 'M': win_open(WIN_MEMINFO); need_redraw = 1; break;
            case 't': case 'T': win_open(WIN_TASKS);   need_redraw = 1; break;
            case 'f': case 'F': win_open(WIN_FILES);   need_redraw = 1; break;
            case '\t': {                                     /* cycle focus */
                int open = 0;
                for (int i = 0; i < MAX_WIN; i++) if (wins[i].used) open++;
                if (open > 0) {
                    /* the old loop spun forever when nothing was open,
                     * because focus started at -1 */
                    int i = focus;
                    for (int k = 0; k < MAX_WIN; k++) {
                        i = (i + 1) % MAX_WIN;
                        if (wins[i].used) break;
                    }
                    win_raise(i);
                    need_redraw = 1;
                }
                break;
            }
            default: break;
            }
        }

        /* ---- repaint only when something moved ---- */
        if (!need_redraw && (in.mx != last_mx || in.my != last_my)) {
            need_redraw = 1;             /* cursor moved: must erase old pos */
        }
        if (need_redraw) {
            redraw_all();
            need_redraw = 0;
        }
        draw_cursor(&in);
        last_mx = in.mx;
        last_my = in.my;

        hlt();   /* the old loop busy-spun at 100% CPU */
    }

    serial_puts("[gui] leaving desktop\n");
}
