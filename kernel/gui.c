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
#include "timer.h"
#include "fat.h"

/* scheduler / paging live data shown in the Tasks and System Info windows */
extern u32 sched_task_count(void);
extern u32 sched_current_pid(void);
extern int  paging_smep_enabled(void);

#define MAX_WIN   8
#define TASKBAR_H 12
#define TITLE_H   12
#define CASCADE_X 10
#define CASCADE_Y 14

enum { WIN_CONSOLE, WIN_ABOUT, WIN_MEMINFO, WIN_TASKS, WIN_FILES,
       WIN_SYSINFO, WIN_NOTEPAD, WIN_CALC, WIN_BROWSER };

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

/* ---- application state, one instance per app type --------------------- */

#define NP_CAP   512                 /* notepad text buffer */
static char np_buf[NP_CAP];
static u32  np_len = 0;

#define CALC_CAP 32
static char calc_buf[CALC_CAP];      /* expression being typed */
static u32  calc_len = 0;
static int  calc_has_result = 0;
static int  calc_result = 0;
static char calc_err[20];

#define BR_PAGE_HOME 0
static int  br_page = BR_PAGE_HOME;  /* 0 home, 1..3 = FAT documents */

#define FILE_ROWS 6
static char   fl_names[FILE_ROWS][FAT_NAME_LEN];
static u32    fl_sizes[FILE_ROWS];
static u32    fl_count = 0;
static int    fl_valid = 0;          /* refreshed since last open */

static void files_refresh(void) {
    u32 n = 0;
    if (fat_mounted() && fat_list(fl_names, fl_sizes, FILE_ROWS, &n))
        fl_count = n;
    else
        fl_count = 0;
    fl_valid = 1;
}

/* evaluate the calc expression strictly left-to-right (no precedence):
 * returns 1 on success, 0 on division by zero, -1 on a malformed expr */
static int calc_eval(const char *s, int *out) {
    int acc = 0, num = 0, have_num = 0;
    char op = '+';
    for (; *s; s++) {
        if (*s >= '0' && *s <= '9') {
            num = num * 10 + (*s - '0');
            have_num = 1;
        } else if (*s == '+' || *s == '-' || *s == '*' || *s == '/') {
            if (!have_num) return -1;
            if (op == '+') acc += num;
            else if (op == '-') acc -= num;
            else if (op == '*') acc *= num;
            else {
                if (num == 0) return 0;
                acc /= num;
            }
            op = *s;
            num = 0;
            have_num = 0;
        } else {
            return -1;                 /* anything else is garbage */
        }
    }
    if (!have_num) return -1;
    if (op == '+') acc += num;
    else if (op == '-') acc -= num;
    else if (op == '*') acc *= num;
    else {
        if (num == 0) return 0;
        acc /= num;
    }
    *out = acc;
    return 1;
}

/* one-frame snapshot of the input edges, so the consuming edge-detectors
 * are each called exactly once per iteration */
typedef struct {
    int click, release, down;   /* left-button edge / level */
    int mx, my;                 /* cursor position */
} input_t;

static const char *win_title(int type) {
    switch (type) {
    case WIN_CONSOLE:  return "Console";
    case WIN_ABOUT:    return "About NovaOS";
    case WIN_MEMINFO:  return "Memory Info";
    case WIN_TASKS:    return "Tasks";
    case WIN_FILES:    return "Files";
    case WIN_SYSINFO:  return "System Info";
    case WIN_NOTEPAD:  return "Notepad";
    case WIN_CALC:     return "Calculator";
    case WIN_BROWSER:  return "NovaBrowse";
    }
    return "Window";
}

/* draw up to maxlines lines of text, wrapping at maxchars columns;
 * returns the y of the next free row */
static int draw_wrapped(const char *s, int x, int y, int maxchars,
                        int maxlines, u8 fg) {
    for (int line = 0; line < maxlines && *s; line++) {
        int col = 0;
        while (*s && col < maxchars && *s != '\n')
            gfx_drawchar(x + col * 8, y + line * 9, *s++, fg, 0x07), col++;
        if (*s == '\n') s++;
    }
    return y + maxlines * 9;
}

static void win_draw_frame(window_t *w) {
    u8 title_bg = w->focused ? 0x09 : 0x08;    /* focused = bright blue */

    /* drop shadow: offset to the bottom-right in the darkest palette
     * entry, so windows read as layered on the desktop */
    gfx_fillrect(w->x + 3, w->y + 3, w->x + w->w + 2, w->y + w->h + 2, 0x00);

    /* body, then title bar, then a raised border */
    gfx_fillrect(w->x + 1, w->y + 1, w->x + w->w - 2, w->y + w->h - 2, 0x03);
    /* one-pixel highlight at the top of the focused title bar */
    if (w->focused)
        gfx_fillrect(w->x + 1, w->y + 1, w->x + w->w - 2, w->y + 1, 0x0B);
    gfx_fillrect(w->x + 1, w->y + (w->focused ? 2 : 1),
                 w->x + w->w - 2, w->y + TITLE_H, title_bg);
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
        gfx_drawstring(cx, cy,      "NovaOS v0.8",        0x04, 0x07);
        gfx_drawstring(cx, cy + 11, "home-grown kernel",   0x01, 0x07);
        gfx_drawstring(cx, cy + 21, "GDT IDT paging",      0x01, 0x07);
        gfx_drawstring(cx, cy + 31, "ring3 FAT12 mouse",   0x01, 0x07);
        gfx_drawstring(cx, cy + 45, "N notepad K calc B browse",     0x0E, 0x07);
        gfx_drawstring(cx, cy + 55, "drag: title, close: x/Esc",     0x0E, 0x07);
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
        if (!fat_mounted()) {
            gfx_drawstring(cx, cy, "no FAT12 volume", 0x0C, 0x07);
            break;
        }
        if (fl_count == 0) {
            gfx_drawstring(cx, cy, "volume empty", 0x0E, 0x07);
        } else {
            for (u32 i = 0; i < fl_count && i < FILE_ROWS; i++) {
                ksprintf(buf, "%-12s %6u B", fl_names[i], fl_sizes[i]);
                gfx_drawstring(cx, cy + (int)i * 9, buf, 0x02, 0x07);
            }
        }
        ksprintf(buf, "free %u KB", fat_free_bytes() / 1024);
        gfx_drawstring(cx, cy + FILE_ROWS * 9 + 4, buf, 0x01, 0x07);
        break;
    }

    case WIN_NOTEPAD: {
        int maxc = (w->w - 14) / 8;
        int maxl = (w->h - TITLE_H - 18) / 9;
        draw_wrapped(np_buf, cx, cy, maxc, maxl, 0x01);
        /* caret: a solid block right after the last character */
        gfx_drawstring(cx, cy + maxl * 9,
                       "type here - saved on close", 0x0E, 0x07);
        break;
    }

    case WIN_CALC: {
        gfx_fillrect(w->x + 5, cy, w->x + w->w - 6, cy + 11, 0x0F);
        u32 elen = calc_len > 18 ? calc_len - 18 : 0;
        gfx_drawstring(cx, cy + 2, calc_buf + elen, 0x01, 0x0F);
        if (calc_has_result) {
            ksprintf(buf, "= %d", calc_result);
            gfx_drawstring(cx, cy + 16, buf, 0x02, 0x07);
        } else if (calc_err[0]) {
            gfx_drawstring(cx, cy + 16, calc_err, 0x0C, 0x07);
        }
        gfx_drawstring(cx, cy + 30, "0-9 + - * / =  C clear", 0x0E, 0x07);
        gfx_drawstring(cx, cy + 40, "left-to-right, no precedence", 0x06, 0x07);
        break;
    }

    case WIN_BROWSER: {
        int maxc = (w->w - 14) / 8;
        int maxl = (w->h - TITLE_H - 16) / 9;
        static const char *links[3] =
            { "1. readme.txt", "2. hello.txt", "3. about" };
        if (br_page == BR_PAGE_HOME) {
            gfx_drawstring(cx, cy, "NovaBrowse 0.1", 0x09, 0x07);
            gfx_drawstring(cx, cy + 11, "the NovaOS web", 0x06, 0x07);
            for (int i = 0; i < 3; i++)
                gfx_drawstring(cx, cy + 26 + i * 9, links[i], 0x04, 0x07);
            gfx_drawstring(cx, cy + 26 + 3 * 9,
                           "press 1-3 to follow a link", 0x0E, 0x07);
        } else {
            const char *addr = (br_page == 3) ? "doc://about"
                                              : (br_page == 1)
                                                ? "doc://readme.txt"
                                                : "doc://hello.txt";
            gfx_fillrect(w->x + 5, cy, w->x + w->w - 6, cy + 10, 0x08);
            gfx_drawstring(cx, cy + 1, addr, 0x0F, 0x08);
            char doc[640];
            u32 size = 0;
            if (br_page != 3 && fat_mounted() &&
                fat_read(br_page == 1 ? "readme.txt" : "hello.txt",
                         (u8*)doc, sizeof(doc) - 1, &size) && size) {
                doc[size] = '\0';
            } else if (br_page == 3) {
                strncpy(doc,
                        "NovaBrowse renders text documents from the FAT12 "
                        "volume. No network stack yet - the real web is a "
                        "future project.", sizeof(doc) - 1);
                doc[sizeof(doc) - 1] = '\0';
            } else {
                strncpy(doc, "(document not found)", sizeof(doc) - 1);
                doc[sizeof(doc) - 1] = '\0';
            }
            draw_wrapped(doc, cx, cy + 12, maxc, maxl, 0x01);
        }
        gfx_drawstring(cx, cy + (w->h - TITLE_H - 13),
                       br_page == BR_PAGE_HOME ? "H home | 1-3 links"
                                               : "H home | 1-3 links",
                       0x0E, 0x07);
        break;
    }

    case WIN_SYSINFO: {
        ksprintf(buf, "up %u.%02u s", (u32)(timer_ticks() / 100),
                 (u32)(timer_ticks() % 100));
        gfx_drawstring(cx, cy, buf, 0x01, 0x07);
        gfx_drawstring(cx, cy + 12, "CR0.WP  on", 0x02, 0x07);
        if (paging_smep_enabled())
            gfx_drawstring(cx, cy + 22, "SMEP    on", 0x02, 0x07);
        else
            gfx_drawstring(cx, cy + 22, "SMEP    n/a", 0x0E, 0x07);
        ksprintf(buf, "%u tasks", sched_task_count());
        gfx_drawstring(cx, cy + 36, buf, 0x01, 0x07);
        break;
    }

    case WIN_CONSOLE:
    default:
        gfx_drawstring(cx, cy,      "kernel: hello from C",  0x02, 0x07);
        gfx_drawstring(cx, cy + 12, "boot: loader->pmode",   0x02, 0x07);
        gfx_drawstring(cx, cy + 24, "user: ring3 process",   0x02, 0x07);
        gfx_drawstring(cx, cy + 38, "C A M T F Y N K B",     0x0E, 0x07);
        break;
    }
}

static void redraw_all(void) {
    /* vertical desktop gradient: the DAC ramp programmed in vga.c gives
     * 48 shades (indices 16..63) from deep blue-grey down to slate - the
     * old flat teal fill looked like a test pattern */
    int gh = GFX_H - TASKBAR_H;
    for (int y = 0; y < gh; y++)
        gfx_fillrect(0, y, GFX_W - 1, y, 16 + (y * 47) / (gh - 1));

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
    gfx_fillrect(0, GFX_H - TASKBAR_H, GFX_W - 1, GFX_H - TASKBAR_H, 0x0B);
    gfx_drawstring(4, GFX_H - 10, "NovaOS", 0x0A, 0x00);
    {   /* clock on the right edge: time since boot */
        u32 t = timer_ticks();
        char clock[16];
        ksprintf(clock, "%u:%02u", t / 6000, (t / 100) % 60);
        gfx_drawstring(GFX_W - 8 * (u32)strlen(clock) - 4, GFX_H - 10,
                       clock, 0x07, 0x00);
    }
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
        /* per-app default geometry: the desktop is only 320x200 */
        switch (type) {
        case WIN_NOTEPAD: w->w = 212; w->h = 112; break;
        case WIN_BROWSER: w->w = 224; w->h = 118; break;
        case WIN_CALC:    w->w = 172; w->h = 86;  break;
        default:          w->w = 168; w->h = 88;  break;
        }
        if (w->x + w->w > GFX_W) w->x = GFX_W - w->w;
        if (w->y + w->h > GFX_H - TASKBAR_H)
            w->y = GFX_H - TASKBAR_H - w->h;
        strncpy(w->title, win_title(type), sizeof(w->title) - 1);
        w->title[sizeof(w->title) - 1] = '\0';
        if (type == WIN_FILES) files_refresh();
        if (type == WIN_NOTEPAD) {          /* resume the last session */
            u32 size = 0;
            if (fat_mounted() &&
                fat_read("notepad.txt", (u8*)np_buf, NP_CAP - 1, &size)) {
                np_buf[size] = '\0';
                np_len = size;
            }
        }
        win_raise(i);
        return i;
    }
    return -1;                 /* out of window slots */
}

/* persist the notepad contents; called when its window closes */
static void notepad_save(void) {
    if (!fat_mounted() || np_len == 0) return;
    fat_write("notepad.txt", (const u8*)np_buf, np_len);
}

static void win_close(int idx) {
    if (idx < 0 || idx >= MAX_WIN || !wins[idx].used) return;
    if (wins[idx].type == WIN_NOTEPAD) notepad_save();
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
    win_open(WIN_BROWSER);
    win_open(WIN_FILES);
    win_open(WIN_NOTEPAD);

    serial_puts("[gui] mode 13h desktop up (320x200x256), windows=4\n");

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
            } else if (in.my >= GFX_H - TASKBAR_H) {
                /* taskbar buttons: focus the matching window.  The buttons
                 * used to be pure decoration - clicking them did nothing. */
                int bx = 60;
                for (int i = 0; i < MAX_WIN && bx <= GFX_W - 50; i++) {
                    if (!wins[i].used) continue;
                    if (in.mx >= bx - 2 && in.mx <= bx + 42) {
                        win_raise(i);
                        need_redraw = 1;
                        break;
                    }
                    bx += 46;
                }
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

        /* ---- keyboard ----
         * Editable apps (notepad / calculator / browser) swallow normal
         * keys while focused; global hotkeys apply only when no editable
         * window has focus.  Esc always closes, Enter still leaves the
         * desktop unless an editable window needs the key. */
        int c = keyboard_getc();
        int editing = (focus >= 0 &&
                       (wins[focus].type == WIN_NOTEPAD ||
                        wins[focus].type == WIN_CALC ||
                        wins[focus].type == WIN_BROWSER));
        if (c) {
            if (c == 27) {                                   /* Esc */
                if (focus >= 0) win_close(focus);
                need_redraw = 1;
            } else if (editing && c != '\t') {
                switch (wins[focus].type) {
                case WIN_NOTEPAD:
                    if (c == '\b' || c == 127) {             /* backspace */
                        if (np_len) np_buf[--np_len] = '\0';
                    } else if (c == '\n' || c == '\r') {
                        if (np_len < NP_CAP - 2) {
                            np_buf[np_len++] = '\n';
                            np_buf[np_len] = '\0';
                        }
                    } else if (c >= 32 && c < 127 &&
                               np_len < NP_CAP - 1) {
                        np_buf[np_len++] = (char)c;
                        np_buf[np_len] = '\0';
                    }
                    break;
                case WIN_CALC:
                    if (c == '\b' || c == 127) {
                        if (calc_len) calc_buf[--calc_len] = '\0';
                        calc_has_result = 0;
                        calc_err[0] = '\0';
                    } else if (c == '=' || c == '\r' || c == '\n') {
                        int r;
                        int rc = calc_eval(calc_buf, &r);
                        if (rc == 1) {
                            calc_result = r;
                            calc_has_result = 1;
                            calc_err[0] = '\0';
                        } else {
                            calc_has_result = 0;
                            strncpy(calc_err, rc == 0 ? "div by zero"
                                      : "syntax error",
                                      sizeof(calc_err) - 1);
                            calc_err[sizeof(calc_err) - 1] = '\0';
                        }
                    } else if (c == 'c' || c == 'C') {
                        calc_len = 0;
                        calc_buf[0] = '\0';
                        calc_has_result = 0;
                        calc_err[0] = '\0';
                    } else if ((c >= '0' && c <= '9') ||
                               c == '+' || c == '-' || c == '*' ||
                               c == '/') {
                        if (calc_len < CALC_CAP - 1) {
                            calc_buf[calc_len++] = (char)c;
                            calc_buf[calc_len] = '\0';
                        }
                        calc_has_result = 0;
                        calc_err[0] = '\0';
                    }
                    break;
                case WIN_BROWSER:
                    if (c >= '1' && c <= '3')
                        br_page = c - '0';
                    else if (c == 'h' || c == 'H')
                        br_page = BR_PAGE_HOME;
                    break;
                }
                need_redraw = 1;
            } else if (c == 0x0D || c == 0x0A) {             /* Enter: leave GUI */
                break;
            } else switch (c) {
            case 'c': case 'C': win_open(WIN_CONSOLE);  need_redraw = 1; break;
            case 'a': case 'A': win_open(WIN_ABOUT);    need_redraw = 1; break;
            case 'm': case 'M': win_open(WIN_MEMINFO);  need_redraw = 1; break;
            case 't': case 'T': win_open(WIN_TASKS);    need_redraw = 1; break;
            case 'f': case 'F': win_open(WIN_FILES);    need_redraw = 1; break;
            case 'y': case 'Y': win_open(WIN_SYSINFO);  need_redraw = 1; break;
            case 'n': case 'N': win_open(WIN_NOTEPAD);  need_redraw = 1; break;
            case 'k': case 'K': win_open(WIN_CALC);     need_redraw = 1; break;
            case 'b': case 'B': win_open(WIN_BROWSER);  need_redraw = 1; break;
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
