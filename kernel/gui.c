/* NovaOS 1.0 "Sun" - modern desktop shell
 *
 * Replaces the old mode-13h (320x200x256) desktop with a true-colour
 * compositor on top of the VBE linear framebuffer:
 *
 *   - wallpaper with a vertical gradient and soft accent glows
 *   - top bar with the product name and a live RTC clock
 *   - floating rounded windows with drop shadows and traffic-light buttons
 *   - a centred dock with hand-drawn icons
 *   - a real text editor (open / edit / save on the FAT12 volume)
 *
 * Everything is drawn with the immediate-mode gfx_* API in vga.c, so the
 * whole shell is a few hundred lines of plain C - no toolkit, no libs.
 */
#include "gui.h"
#include "vga.h"
#include "vbe.h"
#include "keyboard.h"
#include "mouse.h"
#include "string.h"
#include "printf.h"
#include "serial.h"
#include "mm.h"
#include "kheap.h"
#include "timer.h"
#include "rtc.h"
#include "fat.h"
#include "io.h"

extern u32 sched_task_count(void);
extern u32 sched_current_pid(void);
extern int paging_smep_enabled(void);

/* ------------------------------------------------------------------ */
/* palette                                                            */
/* ------------------------------------------------------------------ */
#define C_BG_TOP   RGB(15, 21, 38)
#define C_BG_BOT   RGB(7, 10, 18)
#define C_TOPBAR   RGB(17, 24, 39)
#define C_DOCK     RGB(23, 32, 50)
#define C_WIN      RGB(26, 36, 54)
#define C_WIN2     RGB(32, 44, 66)
#define C_TITLE    RGB(36, 50, 74)
#define C_TITLE2   RGB(28, 39, 58)
#define C_BORDER   RGB(55, 72, 102)
#define C_TEXT     RGB(233, 238, 247)
#define C_DIM      RGB(154, 170, 194)
#define C_MUTED    RGB(106, 124, 150)
#define C_OK       RGB(34, 197, 94)
#define C_WARN     RGB(245, 158, 11)
#define C_ERR      RGB(239, 68, 68)
#define C_CYAN     RGB(34, 211, 238)

static u32 darken(u32 c, int a) {
    u32 r = ((c >> 16) & 0xFF) * (u32)a / 255;
    u32 g = ((c >> 8) & 0xFF) * (u32)a / 255;
    u32 b = (c & 0xFF) * (u32)a / 255;
    return RGB(r, g, b);
}
static u32 accent = RGB(59, 130, 246);
static u32 accent_dim = RGB(37, 82, 158);

/* ------------------------------------------------------------------ */
/* layout (scaled from a 1024x768 design)                             */
/* ------------------------------------------------------------------ */
static int W, H, SC, SM;
#define PX(v) ((v) * H / 768)
#define TOPH  PX(40)
#define DOCKH PX(74)
#define RAD   PX(14)
#define TH    PX(44)

/* ------------------------------------------------------------------ */
/* windows                                                            */
/* ------------------------------------------------------------------ */
#define MAX_WIN 8
enum { APP_EDITOR = 0, APP_FILES, APP_CONSOLE, APP_CALC,
       APP_SETTINGS, APP_ABOUT, APP_COUNT };

typedef struct {
    int used, app, x, y, w, h;
    int min;                    /* minimised */
} win_t;

static win_t wins[MAX_WIN];
static int focus = -1;

static const char *app_title(int a) {
    switch (a) {
    case APP_EDITOR:   return "Text Editor";
    case APP_FILES:    return "Files";
    case APP_CONSOLE:  return "Console";
    case APP_CALC:     return "Calculator";
    case APP_SETTINGS: return "Settings";
    case APP_ABOUT:    return "About NovaOS";
    }
    return "Window";
}

/* ------------------------------------------------------------------ */
/* buttons (rebuilt every frame)                                      */
/* ------------------------------------------------------------------ */
#define MAXBTN 40
typedef struct { int x, y, w, h, id; } btn_t;
static btn_t btns[MAXBTN];
static int nbtn = 0, sel = -1;

static void btn_add(int x, int y, int w, int h, int id) {
    if (nbtn >= MAXBTN) return;
    btns[nbtn].x = x; btns[nbtn].y = y;
    btns[nbtn].w = w; btns[nbtn].h = h;
    btns[nbtn].id = id;
    nbtn++;
}
static int btn_hit(int mx, int my) {
    for (int i = nbtn - 1; i >= 0; i--)
        if (mx >= btns[i].x && mx < btns[i].x + btns[i].w &&
            my >= btns[i].y && my < btns[i].y + btns[i].h)
            return btns[i].id;
    return -1;
}

/* ------------------------------------------------------------------ */
/* editor state                                                       */
/* ------------------------------------------------------------------ */
#define ED_CAP 8000
static char ed_buf[ED_CAP];
static int  ed_len = 0, ed_cx = 0, ed_cy = 0, ed_top = 0;
static int  ed_dirty = 0, ed_pick = 0;
static char ed_name[16] = "NOTES.TXT";
static char ed_msg[64] = "";
static int  ed_text_x = 0, ed_text_y = 0, ed_lh = 0, ed_rows = 0;
static int  ed_cols = 0;

/* file picker list */
#define PK_N 12
static char pk_names[PK_N][FAT_NAME_LEN];
static u32  pk_sizes[PK_N];
static int  pk_n = 0, pk_sel = 0;

/* ------------------------------------------------------------------ */
/* calculator state                                                   */
/* ------------------------------------------------------------------ */
static char calc_buf[24];
static int  calc_len = 0, calc_ok = 0, calc_val = 0;
static char calc_err[20];

/* ------------------------------------------------------------------ */
/* console state                                                      */
/* ------------------------------------------------------------------ */
static const char *con_lines[] = {
    "NovaOS 1.0 (Sun) - home-grown kernel",
    "",
    "stage1 -> stage2 -> pmode -> paging -> ring3",
    "GDT  IDT  paging  COW fork  FAT12  scheduler",
    "",
    "This shell is C + GNU as, drawn straight to",
    "the VBE framebuffer. No libraries involved.",
    "",
    "F1  editor    F2 save    F3 open    F4 new",
    "Esc closes the focused window",
};
#define CON_N ((int)(sizeof con_lines / sizeof con_lines[0]))

/* ------------------------------------------------------------------ */
/* helpers                                                            */
/* ------------------------------------------------------------------ */
static void text_c(int cx, int y, const char *s, u32 fg, int sc) {
    gfx_str(cx - gfx_strw(s, sc) / 2, y, s, fg, sc);
}

static int win_at(int x, int y) {
    if (focus >= 0 && wins[focus].used && !wins[focus].min &&
        x >= wins[focus].x && x < wins[focus].x + wins[focus].w &&
        y >= wins[focus].y && y < wins[focus].y + wins[focus].h)
        return focus;
    for (int i = MAX_WIN - 1; i >= 0; i--) {
        if (!wins[i].used || wins[i].min) continue;
        if (x >= wins[i].x && x < wins[i].x + wins[i].w &&
            y >= wins[i].y && y < wins[i].y + wins[i].h)
            return i;
    }
    return -1;
}

static void win_raise(int i) {
    if (i < 0 || i >= MAX_WIN || !wins[i].used) return;
    focus = i;
    wins[i].min = 0;
}

static int win_open(int app) {
    for (int i = 0; i < MAX_WIN; i++) {
        if (wins[i].used && wins[i].app == app) { win_raise(i); return i; }
    }
    for (int i = 0; i < MAX_WIN; i++) {
        if (wins[i].used) continue;
        win_t *w = &wins[i];
        w->used = 1;
        w->app = app;
        w->min = 0;
        int n = 0;
        for (int k = 0; k < MAX_WIN; k++) if (wins[k].used) n++;

        switch (app) {
        case APP_EDITOR:   w->w = PX(760); w->h = PX(520); break;
        case APP_FILES:    w->w = PX(560); w->h = PX(430); break;
        case APP_CONSOLE:  w->w = PX(660); w->h = PX(400); break;
        case APP_CALC:     w->w = PX(380); w->h = PX(470); break;
        case APP_SETTINGS: w->w = PX(600); w->h = PX(430); break;
        default:           w->w = PX(520); w->h = PX(360); break;
        }
        if (w->w > W - PX(40)) w->w = W - PX(40);
        if (w->h > H - TOPH - DOCKH - PX(20)) w->h = H - TOPH - DOCKH - PX(20);
        w->x = PX(60) + (n % 5) * PX(28);
        w->y = TOPH + PX(34) + (n % 5) * PX(26);
        if (w->x + w->w > W - PX(20)) w->x = W - PX(20) - w->w;
        if (w->y + w->h > H - DOCKH - PX(10)) w->y = H - DOCKH - PX(10) - w->h;
        focus = i;
        return i;
    }
    return -1;
}

static void win_close(int i) {
    if (i < 0 || i >= MAX_WIN) return;
    wins[i].used = 0;
    focus = -1;
    for (int k = MAX_WIN - 1; k >= 0; k--)
        if (wins[k].used && !wins[k].min) { focus = k; break; }
}

/* ------------------------------------------------------------------ */
/* wallpaper + chrome                                                 */
/* ------------------------------------------------------------------ */
static void draw_glow(int cx, int cy, int r, u32 col, int a0) {
    for (int i = 0; i < 6; i++) {
        int rr = r - i * (r / 8);
        gfx_round_a(cx - rr, cy - rr, cx + rr, cy + rr, rr, col, a0 / 6);
    }
}

static void wallpaper(void) {
    gfx_grad(0, 0, W - 1, H - 1, C_BG_TOP, C_BG_BOT);
    draw_glow(W * 3 / 4, H / 5, PX(240), accent, 42);
    draw_glow(W / 5, H * 3 / 4, PX(210), RGB(120, 60, 200), 26);
}

static void topbar(void) {
    gfx_rect(0, 0, W - 1, TOPH, C_TOPBAR);
    gfx_rect(0, TOPH, W - 1, TOPH + 1, RGB(40, 54, 78));

    /* logo dot + product name */
    int cy = TOPH / 2;
    gfx_round(PX(16), cy - PX(6), PX(16) + PX(12), cy + PX(6), PX(6), accent);
    gfx_str(PX(36), cy - gfx_strh(SC) / 2, "NovaOS", C_TEXT, SC);
    gfx_str(PX(36) + gfx_strw("NovaOS", SC) + PX(8),
            cy - gfx_strh(SM) / 2, "1.1 Prism", C_MUTED, SM);

    /* clock from the CMOS RTC */
    rtc_time t;
    char clk[24] = "--:--";
    char day[32] = "";
    if (rtc_get(&t)) {
        ksprintf(clk, "%02u:%02u", (u32)t.hour, (u32)t.min);
        static const char *mon[] = { "Jan","Feb","Mar","Apr","May","Jun",
                                     "Jul","Aug","Sep","Oct","Nov","Dec" };
        const char *mn = (t.mon >= 1 && t.mon <= 12) ? mon[t.mon - 1] : "---";
        ksprintf(day, "%s %u %u", mn, (u32)t.day, t.year);
    }
    int cw = gfx_strw(clk, SC);
    gfx_str(W - PX(18) - cw, cy - gfx_strh(SC) / 2, clk, C_TEXT, SC);
    gfx_str(W - PX(18) - cw - PX(14) - gfx_strw(day, SM),
            cy - gfx_strh(SM) / 2, day, C_MUTED, SM);
}

/* dock icon kinds */
enum { IC_EDITOR, IC_FILES, IC_CONSOLE, IC_CALC, IC_SETTINGS, IC_ABOUT };

static void draw_icon(int kind, int cx, int cy, int s, u32 col) {
    int h = s / 2;
    switch (kind) {
    case IC_EDITOR:
        gfx_round(cx - h, cy - h, cx + h, cy + h, PX(4), col);
        for (int i = -2; i <= 2; i++)
            gfx_rect(cx - h + PX(5), cy + i * PX(4), cx + h - PX(5),
                     cy + i * PX(4), RGB(12, 18, 30));
        break;
    case IC_FILES:
        gfx_round(cx - h, cy - h + PX(3), cx + h - PX(2), cy + h, PX(4), col);
        gfx_round(cx - h, cy - h, cx - PX(2), cy - h + PX(7), PX(3), col);
        break;
    case IC_CONSOLE:
        gfx_round(cx - h, cy - h, cx + h, cy + h, PX(4), col);
        gfx_str(cx - h + PX(4), cy - PX(6), ">_", RGB(12, 18, 30), 1);
        break;
    case IC_CALC:
        gfx_round(cx - h, cy - h, cx + h, cy + h, PX(4), col);
        gfx_rect(cx - h + PX(4), cy - h + PX(4), cx + h - PX(4), cy - h + PX(8),
                 RGB(12, 18, 30));
        for (int r = 0; r < 2; r++)
            for (int c = 0; c < 3; c++)
                gfx_rect(cx - h + PX(5) + c * PX(6), cy + PX(1) + r * PX(6),
                         cx - h + PX(8) + c * PX(6), cy + PX(3) + r * PX(6),
                         RGB(12, 18, 30));
        break;
    case IC_SETTINGS: {
        gfx_round(cx - h, cy - h, cx + h, cy + h, h / 2 + 1, col);
        gfx_round(cx - PX(4), cy - PX(4), cx + PX(4), cy + PX(4), PX(4),
                  RGB(14, 20, 34));
        break;
    }
    default:
        gfx_round(cx - h, cy - h, cx + h, cy + h, h / 2 + 1, col);
        gfx_str(cx - PX(2), cy - PX(7), "i", RGB(14, 20, 34), 2);
        break;
    }
}

static void dock(void) {
    int n = APP_COUNT;
    int isz = PX(46), gap = PX(14);
    int dw = n * isz + (n + 1) * gap;
    int dx = (W - dw) / 2;
    int dy = H - DOCKH + PX(10);
    int dh = DOCKH - PX(20);

    gfx_round_a(dx, dy + PX(4), dx + dw, dy + dh + PX(4), dh / 2,
                RGB(0, 0, 0), 70);
    gfx_round(dx, dy, dx + dw, dy + dh, dh / 2, C_DOCK);
    gfx_ring(dx, dy, dx + dw, dy + dh, dh / 2, RGB(48, 64, 90));

    int iy = dy + dh / 2;
    for (int i = 0; i < n; i++) {
        int ix = dx + gap + isz / 2 + i * (isz + gap);
        int hover = btn_hit(mouse_pos_x(), mouse_pos_y()) == 900 + i;
        u32 c = (i == focus && wins[i].used) ? accent
                                             : RGB(70, 92, 126);
        if (hover) c = (c == accent) ? RGB(96, 165, 250) : RGB(92, 116, 156);
        draw_icon(i, ix, iy, isz, c);
        btn_add(ix - isz / 2, iy - isz / 2, isz, isz, 900 + i);
    }
}

/* ------------------------------------------------------------------ */
/* window frame                                                       */
/* ------------------------------------------------------------------ */
static void win_frame(win_t *w, int focused) {
    int x = w->x, y = w->y, ww = w->w, hh = w->h;

    /* layered shadow */
    for (int i = PX(8); i >= PX(2); i -= PX(2))
        gfx_round_a(x - i, y - i + PX(6), x + ww + i, y + hh + i + PX(6),
                    RAD + i, RGB(0, 0, 0), 18);

    gfx_round(x, y, x + ww, y + hh, RAD, C_WIN);
    gfx_ring(x, y, x + ww, y + hh, RAD, focused ? accent_dim : RGB(44, 58, 82));

    /* title bar: rounded top, square bottom */
    u32 tb = focused ? C_TITLE : C_TITLE2;
    gfx_round(x + 1, y + 1, x + ww - 1, y + TH + RAD, RAD, tb);
    gfx_rect(x + 1, y + RAD, x + ww - 1, y + TH, tb);
    gfx_rect(x + 1, y + TH, x + ww - 1, y + TH + 1,
             focused ? accent : RGB(44, 58, 82));

    /* traffic lights */
    int cy = y + TH / 2, r = PX(7);
    gfx_round(x + PX(16) - r, cy - r, x + PX(16) + r, cy + r, r, C_ERR);
    gfx_round(x + PX(38) - r, cy - r, x + PX(38) + r, cy + r, r, C_WARN);
    gfx_round(x + PX(60) - r, cy - r, x + PX(60) + r, cy + r, r, C_OK);

    gfx_str(x + PX(82), cy - gfx_strh(SC) / 2, app_title(w->app),
            focused ? C_TEXT : C_DIM, SC);
}

/* ------------------------------------------------------------------ */
/* editor                                                             */
/* ------------------------------------------------------------------ */
static int ed_lines(void) {
    int n = 1;
    for (int i = 0; i < ed_len; i++) if (ed_buf[i] == '\n') n++;
    return n;
}
static int ed_line_off(int line) {
    int n = 0, off = 0;
    for (int i = 0; i < ed_len; i++) {
        if (n == line) return off;
        if (ed_buf[i] == '\n') { n++; off = i + 1; }
    }
    return ed_len;
}
static int ed_line_len(int line) {
    int o = ed_line_off(line), e = o;
    while (e < ed_len && ed_buf[e] != '\n') e++;
    return e - o;
}
static int ed_index(void) { return ed_line_off(ed_cy) + ed_cx; }

static void ed_clamp_cx(void) {
    int l = ed_line_len(ed_cy);
    if (ed_cx > l) ed_cx = l;
    if (ed_cx < 0) ed_cx = 0;
}

static void ed_insert(char c) {
    if (ed_len >= ED_CAP - 1) return;
    int idx = ed_index();
    for (int i = ed_len; i > idx; i--) ed_buf[i] = ed_buf[i - 1];
    ed_buf[idx] = c;
    ed_len++;
    if (c == '\n') { ed_cy++; ed_cx = 0; }
    else ed_cx++;
    ed_dirty = 1;
    ed_msg[0] = '\0';
}

static void ed_backspace(void) {
    if (ed_len == 0) return;
    if (ed_cx == 0 && ed_cy == 0) return;
    int idx = ed_index();
    if (ed_cx == 0) {                       /* join with the previous line */
        int prev = ed_line_len(ed_cy - 1);
        for (int i = idx; i < ed_len; i++) ed_buf[i - 1] = ed_buf[i];
        ed_len--;
        ed_cy--;
        ed_cx = prev;
    } else {
        for (int i = idx; i < ed_len; i++) ed_buf[i - 1] = ed_buf[i];
        ed_len--;
        ed_cx--;
    }
    ed_dirty = 1;
    ed_msg[0] = '\0';
}

static void ed_delete(void) {
    if (ed_len == 0) return;
    int idx = ed_index();
    if (idx >= ed_len) return;
    for (int i = idx; i < ed_len; i++) ed_buf[i] = ed_buf[i + 1];
    ed_len--;
    ed_dirty = 1;
    ed_msg[0] = '\0';
}

static void ed_save(void) {
    if (!fat_mounted()) {
        strncpy(ed_msg, "no FAT volume", sizeof ed_msg - 1);
        ed_msg[sizeof ed_msg - 1] = '\0';
        return;
    }
    if (fat_write(ed_name, (const u8*)ed_buf, (u32)ed_len)) {
        ed_dirty = 0;
        strncpy(ed_msg, "saved", sizeof ed_msg - 1);
        ed_msg[sizeof ed_msg - 1] = '\0';
    } else {
        strncpy(ed_msg, "save failed", sizeof ed_msg - 1);
        ed_msg[sizeof ed_msg - 1] = '\0';
    }
}

static void ed_load(const char *name) {
    u32 size = 0;
    if (!fat_mounted()) return;
    if (fat_read(name, (u8*)ed_buf, ED_CAP - 1, &size)) {
        ed_len = (int)size;
        ed_buf[ed_len] = '\0';
        strncpy(ed_name, name, sizeof ed_name - 1);
        ed_name[sizeof ed_name - 1] = '\0';
        ed_cx = ed_cy = ed_top = 0;
        ed_dirty = 0;
        ed_pick = 0;
        strncpy(ed_msg, "opened", sizeof ed_msg - 1);
        ed_msg[sizeof ed_msg - 1] = '\0';
    }
}

static void ed_new(void) {
    ed_len = 0;
    ed_cx = ed_cy = ed_top = 0;
    ed_dirty = 0;
    ed_pick = 0;
    strncpy(ed_name, "NOTES.TXT", sizeof ed_name - 1);
    ed_name[sizeof ed_name - 1] = '\0';
    strncpy(ed_msg, "new file", sizeof ed_msg - 1);
    ed_msg[sizeof ed_msg - 1] = '\0';
}

static void ed_refresh_picker(void) {
    u32 n = 0;
    pk_n = 0;
    if (fat_mounted() && fat_list(pk_names, pk_sizes, PK_N, &n))
        pk_n = (int)n;
    pk_sel = 0;
}

static void draw_editor(win_t *w) {
    int cx = w->x + PX(18);
    int cy = w->y + TH + PX(14);
    int cw = w->w - PX(36);
    int chh = w->h - TH - PX(48);
    char line[96];

    /* toolbar */
    int bh = PX(30);
    const char *labels[3] = { "Save F2", "Open F3", "New F4" };
    int ids[3] = { 100, 101, 102 };
    int bx = cx;
    for (int i = 0; i < 3; i++) {
        int bw = gfx_strw(labels[i], SM) + PX(26);
        gfx_round(bx, cy, bx + bw, cy + bh, PX(7), C_WIN2);
        gfx_ring(bx, cy, bx + bw, cy + bh, PX(7), RGB(58, 76, 104));
        gfx_str(bx + PX(13), cy + (bh - gfx_strh(SM)) / 2, labels[i], C_TEXT, SM);
        btn_add(bx, cy, bw, bh, ids[i]);
        bx += bw + PX(10);
    }
    /* file name on the right of the toolbar */
    {
        char nm[40];
        ksprintf(nm, "%s%s", ed_name, ed_dirty ? " *" : "");
        int nw = gfx_strw(nm, SM);
        gfx_str(cx + cw - nw, cy + (bh - gfx_strh(SM)) / 2, nm,
                ed_dirty ? C_WARN : C_DIM, SM);
    }

    int ty = cy + bh + PX(12);
    int th = cy + chh - ty;

    if (ed_pick) {
        /* ---- file picker ---- */
        gfx_round(cx, ty, cx + cw, ty + th, PX(9), RGB(20, 28, 44));
        gfx_ring(cx, ty, cx + cw, ty + th, PX(9), RGB(48, 64, 90));
        gfx_str(cx + PX(14), ty + PX(12), "Open a file from the FAT volume",
                C_DIM, SM);
        if (pk_n == 0) {
            gfx_str(cx + PX(14), ty + PX(40), "volume empty or not mounted",
                    C_MUTED, SC);
        }
        int rh = PX(34);
        for (int i = 0; i < pk_n && i < 8; i++) {
            int ry = ty + PX(38) + i * rh;
            if (ry + rh > ty + th) break;
            int on = (i == pk_sel);
            if (on) gfx_round(cx + PX(10), ry, cx + cw - PX(10), ry + rh - PX(4),
                              PX(7), accent_dim);
            ksprintf(line, "%-14s  %u B", pk_names[i], pk_sizes[i]);
            gfx_str(cx + PX(22), ry + PX(8), line, on ? C_TEXT : C_DIM, SM);
            btn_add(cx + PX(10), ry, cw - PX(20), rh - PX(4), 200 + i);
        }
        return;
    }

    /* ---- text area ---- */
    gfx_round(cx, ty, cx + cw, ty + th, PX(9), RGB(18, 25, 40));
    gfx_ring(cx, ty, cx + cw, ty + th, PX(9), RGB(46, 62, 88));

    int gutter = PX(44);
    ed_text_x = cx + gutter + PX(10);
    ed_text_y = ty + PX(12);
    ed_lh = gfx_strh(SC) + PX(6);
    ed_rows = th > PX(20) ? (th - PX(22)) / ed_lh : 0;
    ed_cols = (cw - gutter - PX(20)) / (8 * SC);
    if (ed_cols < 1) ed_cols = 1;

    /* keep the cursor visible */
    if (ed_cy < ed_top) ed_top = ed_cy;
    if (ed_cy >= ed_top + ed_rows) ed_top = ed_cy - ed_rows + 1;
    if (ed_top < 0) ed_top = 0;

    int total = ed_lines();
    for (int r = 0; r < ed_rows; r++) {
        int ln = ed_top + r;
        if (ln >= total) break;
        int y = ed_text_y + r * ed_lh;
        ksprintf(line, "%3d", ln + 1);
        gfx_str(cx + PX(10), y, line, RGB(72, 88, 112), SM);

        int off = ed_line_off(ln), len = ed_line_len(ln);
        if (len > ed_cols) len = ed_cols;
        for (int c = 0; c < len; c++) {
            char ch = ed_buf[off + c];
            if (ch < 32) ch = ' ';
            gfx_char(ed_text_x + c * 8 * SC, y, ch, C_TEXT, SC);
        }
        if (ln == ed_cy && ((timer_ticks() / 40) & 1)) {
            int cxp = ed_text_x + ed_cx * 8 * SC;
            gfx_rect(cxp, y, cxp + 8 * SC - 1, y + gfx_strh(SC), accent);
        }
    }

    /* status bar */
    {
        char st[80];
        int total_lines = ed_lines();
        ksprintf(st, "Ln %d, Col %d   %d lines   %d chars%s",
                 ed_cy + 1, ed_cx + 1, total_lines, ed_len,
                 ed_msg[0] ? "   " : "");
        int sy = w->y + w->h - PX(34);
        gfx_str(cx, sy, st, C_MUTED, SM);
        if (ed_msg[0]) {
            gfx_str(cx + gfx_strw(st, SM) + PX(6), sy,
                    ed_msg, C_OK, SM);
        }
    }
}

static void editor_click(win_t *w, int mx, int my) {
    if (ed_pick) return;                     /* picker uses button ids */
    int cx = w->x + PX(18);
    int cy = w->y + TH + PX(14);
    int cw = w->w - PX(36);
    int bh = PX(30);
    int ty = cy + bh + PX(12);
    int th = cy + (w->h - TH - PX(48)) - ty;

    if (mx < ed_text_x || my < ed_text_y) return;
    if (mx > cx + cw || my > ty + th) return;

    int row = (my - ed_text_y) / ed_lh;
    int col = (mx - ed_text_x) / (8 * SC);
    int line = ed_top + row;
    if (line < 0) line = 0;
    if (line >= ed_lines()) line = ed_lines() - 1;
    ed_cy = line;
    ed_cx = col;
    ed_clamp_cx();
}

/* ------------------------------------------------------------------ */
/* files                                                              */
/* ------------------------------------------------------------------ */
#define FL_N 10
static char fl_names[FL_N][FAT_NAME_LEN];
static u32  fl_sizes[FL_N];
static int  fl_n = -1;

static void files_refresh(void) {
    u32 n = 0;
    fl_n = 0;
    if (fat_mounted() && fat_list(fl_names, fl_sizes, FL_N, &n))
        fl_n = (int)n;
}

static void draw_files(win_t *w) {
    int cx = w->x + PX(20);
    int cy = w->y + TH + PX(18);
    int cw = w->w - PX(40);
    char line[96];

    if (fl_n < 0) files_refresh();

    if (!fat_mounted()) {
        gfx_str(cx, cy, "No FAT12 volume attached.", C_WARN, SC);
        gfx_str(cx, cy + PX(26), "Attach data.img as the second disk.", C_MUTED, SM);
        return;
    }
    if (fl_n == 0) {
        gfx_str(cx, cy, "Volume is empty.", C_DIM, SC);
    }
    int rh = PX(42);
    for (int i = 0; i < fl_n && i < 8; i++) {
        int ry = cy + i * (rh + PX(6));
        if (ry + rh > w->y + w->h - PX(50)) break;
        gfx_round(cx, ry, cx + cw, ry + rh, PX(8), C_WIN2);
        gfx_round(cx + PX(10), ry + PX(11), cx + PX(26), ry + rh - PX(8),
                  PX(4), accent_dim);
        gfx_str(cx + PX(36), ry + PX(9), fl_names[i], C_TEXT, SM);
        ksprintf(line, "%u bytes", fl_sizes[i]);
        gfx_str(cx + PX(36), ry + PX(24), line, C_MUTED, SM);
        btn_add(cx, ry, cw, rh, 300 + i);
    }
    ksprintf(line, "free %u KB", fat_free_bytes() / 1024);
    gfx_str(cx, w->y + w->h - PX(34), line, C_MUTED, SM);
}

/* ------------------------------------------------------------------ */
/* console                                                            */
/* ------------------------------------------------------------------ */
static void draw_console(win_t *w) {
    int cx = w->x + PX(20);
    int cy = w->y + TH + PX(16);
    for (int i = 0; i < CON_N; i++) {
        u32 c = (i == 0) ? accent : ((i == 2) ? C_DIM : C_TEXT);
        gfx_str(cx, cy + i * (gfx_strh(SM) + PX(7)), con_lines[i], c, SM);
    }
    char st[96];
    ksprintf(st, "%u tasks   pid %u   uptime %u s   %ux%ux32",
             sched_task_count(), sched_current_pid(),
             (u32)(timer_ticks() / 100), (u32)W, (u32)H);
    gfx_str(cx, w->y + w->h - PX(34), st, C_MUTED, SM);
}

/* ------------------------------------------------------------------ */
/* calculator                                                         */
/* ------------------------------------------------------------------ */
static int calc_eval(const char *s, int *out) {
    int acc = 0, num = 0, have = 0;
    char op = '+';
    for (; *s; s++) {
        if (*s >= '0' && *s <= '9') { num = num * 10 + (*s - '0'); have = 1; }
        else if (*s == '+' || *s == '-' || *s == '*' || *s == '/') {
            if (!have) return -1;
            if (op == '+') acc += num;
            else if (op == '-') acc -= num;
            else if (op == '*') acc *= num;
            else { if (!num) return 0; acc /= num; }
            op = *s; num = 0; have = 0;
        } else return -1;
    }
    if (!have) return -1;
    if (op == '+') acc += num;
    else if (op == '-') acc -= num;
    else if (op == '*') acc *= num;
    else { if (!num) return 0; acc /= num; }
    *out = acc;
    return 1;
}

static void draw_calc(win_t *w) {
    int cx = w->x + PX(20);
    int cw = w->w - PX(40);
    int cy = w->y + TH + PX(18);
    char buf[40];

    gfx_round(cx, cy, cx + cw, cy + PX(64), PX(10), RGB(16, 23, 37));
    gfx_ring(cx, cy, cx + cw, cy + PX(64), PX(10), RGB(48, 64, 90));
    int elen = calc_len > 16 ? calc_len - 16 : 0;
    int ew = gfx_strw(calc_buf + elen, SC);
    gfx_str(cx + cw - PX(14) - ew, cy + PX(12), calc_buf + elen, C_TEXT, SC);
    if (calc_ok) {
        ksprintf(buf, "= %d", calc_val);
        int rw = gfx_strw(buf, SC);
        gfx_str(cx + cw - PX(14) - rw, cy + PX(36), buf, C_OK, SC);
    } else if (calc_err[0]) {
        gfx_str(cx + PX(14), cy + PX(36), calc_err, C_ERR, SM);
    }

    static const char *keys[16] = {
        "7","8","9","/", "4","5","6","*", "1","2","3","-", "0",".","=","+"
    };
    int top = cy + PX(78);
    int gw = (cw - PX(3) * PX(8)) / 4;
    int gh = PX(52);
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            int x = cx + c * (gw + PX(8));
            int y = top + r * (gh + PX(8));
            const char *k = keys[r * 4 + c];
            int isop = (k[0] == '/' || k[0] == '*' || k[0] == '-' ||
                        k[0] == '+' || k[0] == '=');
            gfx_round(x, y, x + gw, y + gh, PX(8),
                      isop ? accent_dim : C_WIN2);
            text_c(x + gw / 2, y + (gh - gfx_strh(SC)) / 2, k, C_TEXT, SC);
            btn_add(x, y, gw, gh, 400 + r * 4 + c);
        }
    }
    gfx_str(cx, w->y + w->h - PX(34), "C clears   Enter evaluates", C_MUTED, SM);
}

/* ------------------------------------------------------------------ */
/* settings / about                                                   */
/* ------------------------------------------------------------------ */
static void draw_settings(win_t *w) {
    int cx = w->x + PX(24);
    int cy = w->y + TH + PX(22);
    char buf[96];

    gfx_str(cx, cy, "Display", accent, SC);
    ksprintf(buf, "%d x %d @ 32bpp   VBE linear framebuffer", W, H);
    gfx_str(cx, cy + PX(26), buf, C_TEXT, SM);
    gfx_str(cx, cy + PX(46), "The mode is programmed by the stage-2 loader", C_MUTED, SM);

    gfx_str(cx, cy + PX(84), "Accent colour", accent, SC);
    const char *names[3] = { "Blue", "Sunset", "Mint" };
    u32 cols[3] = { RGB(59,130,246), RGB(233,84,32), RGB(16,185,129) };
    int bx = cx;
    for (int i = 0; i < 3; i++) {
        int bw = gfx_strw(names[i], SM) + PX(52);
        int on = (accent == cols[i]);
        gfx_round(bx, cy + PX(108), bx + bw, cy + PX(108) + PX(40), PX(9),
                  on ? cols[i] : C_WIN2);
        gfx_round(bx + PX(12), cy + PX(108) + PX(14),
                  bx + PX(12) + PX(12), cy + PX(108) + PX(26), PX(6), cols[i]);
        gfx_str(bx + PX(34), cy + PX(108) + PX(16), names[i],
                on ? RGB(12,18,30) : C_DIM, SM);
        btn_add(bx, cy + PX(108), bw, PX(40), 500 + i);
        bx += bw + PX(12);
    }

    gfx_str(cx, cy + PX(180), "Kernel", accent, SC);
    gfx_str(cx, cy + PX(206), "NovaOS 1.0 (Sun) - C + GNU as, no libraries",
            C_TEXT, SM);
    ksprintf(buf, "heap %u B   free frames %u   tasks %u",
             kheap_used(), mm_free_frames(), sched_task_count());
    gfx_str(cx, cy + PX(226), buf, C_MUTED, SM);
}

static void draw_about(win_t *w) {
    int cx = w->x + w->w / 2;
    int cy = w->y + TH + PX(30);

    gfx_round(cx - PX(34), cy, cx + PX(34), cy + PX(68), PX(18), accent);
    gfx_str(cx - PX(12), cy + PX(22), "N", RGB(12, 18, 30), 5);

    text_c(cx, cy + PX(88), "NovaOS 1.1 Prism", C_TEXT, SC + 1);
    text_c(cx, cy + PX(88) + PX(30), "a home-grown operating system", C_DIM, SM);
    text_c(cx, cy + PX(88) + PX(52),
           "boot sector / kernel / GUI written from scratch", C_MUTED, SM);
    text_c(cx, cy + PX(88) + PX(84), "F1 editor   F2 save   F3 open   F4 new",
           C_WARN, SM);
    text_c(cx, cy + PX(88) + PX(104), "Esc closes a window   Tab cycles focus",
           C_MUTED, SM);
}

/* ------------------------------------------------------------------ */
/* render                                                             */
/* ------------------------------------------------------------------ */
static void render_app(win_t *w) {
    switch (w->app) {
    case APP_EDITOR:   draw_editor(w);   break;
    case APP_FILES:    draw_files(w);    break;
    case APP_CONSOLE:  draw_console(w);  break;
    case APP_CALC:     draw_calc(w);     break;
    case APP_SETTINGS: draw_settings(w); break;
    default:           draw_about(w);    break;
    }
}

static void render_all(void) {
    nbtn = 0;
    wallpaper();

    for (int i = 0; i < MAX_WIN; i++) {
        if (!wins[i].used || wins[i].min || i == focus) continue;
        win_frame(&wins[i], 0);
        render_app(&wins[i]);
    }
    if (focus >= 0 && wins[focus].used && !wins[focus].min) {
        win_frame(&wins[focus], 1);
        render_app(&wins[focus]);
    }

    dock();
    topbar();
}

/* ------------------------------------------------------------------ */
/* cursor with a saved background so moving it does not repaint all   */
/* ------------------------------------------------------------------ */
#define CUR_W 12
#define CUR_H 18
static u32 cur_bak[CUR_H][CUR_W];
static int cur_x = -100, cur_y = -100, cur_valid = 0;

static void cursor_restore(void) {
    if (!cur_valid) return;
    u32 *fb = vbe_fb();
    int p = vbe_pitch_px();
    for (int r = 0; r < CUR_H; r++) {
        int y = cur_y + r;
        if (y < 0 || y >= H) continue;
        for (int c = 0; c < CUR_W; c++) {
            int x = cur_x + c;
            if (x < 0 || x >= W) continue;
            fb[(u32)y * p + x] = cur_bak[r][c];
        }
    }
    cur_valid = 0;
}

static void cursor_draw(int x, int y) {
    u32 *fb = vbe_fb();
    int p = vbe_pitch_px();
    /* save */
    for (int r = 0; r < CUR_H; r++) {
        int yy = y + r;
        for (int c = 0; c < CUR_W; c++) {
            int xx = x + c;
            cur_bak[r][c] = (yy >= 0 && yy < H && xx >= 0 && xx < W)
                          ? fb[(u32)yy * p + xx] : 0;
        }
    }
    cur_x = x; cur_y = y; cur_valid = 1;

    /* arrow: white body, dark outline */
    static const char *shape[CUR_H] = {
        "X           ",
        "XX          ",
        "X.X         ",
        "X..X        ",
        "X...X       ",
        "X....X      ",
        "X.....X     ",
        "X......X    ",
        "X.......X   ",
        "X........X  ",
        "X.....XXXXX ",
        "X..X..X     ",
        "X.X X..X    ",
        "XX  X..X    ",
        "X    X..X   ",
        "     X..X   ",
        "      XX    ",
        "            ",
    };
    for (int r = 0; r < CUR_H; r++) {
        for (int c = 0; c < CUR_W; c++) {
            char ch = shape[r][c];
            if (ch == ' ') continue;
            gfx_px(x + c, y + r, ch == 'X' ? RGB(10, 14, 22) : RGB(248, 250, 253));
        }
    }
}

/* ------------------------------------------------------------------ */
/* input handling                                                     */
/* ------------------------------------------------------------------ */
static void editor_key(int c) {
    if (ed_pick) {
        if (c == 0x148) { if (pk_sel > 0) pk_sel--; }
        else if (c == 0x150) { if (pk_sel < pk_n - 1) pk_sel++; }
        else if (c == '\n' || c == '\r') {
            if (pk_n > 0) ed_load(pk_names[pk_sel]);
        }
        else if (c == 27) ed_pick = 0;
        return;
    }

    switch (c) {
    case 0x148: if (ed_cy > 0) { ed_cy--; ed_clamp_cx(); } break;         /* up */
    case 0x150: if (ed_cy + 1 < ed_lines()) { ed_cy++; ed_clamp_cx(); } break;
    case 0x14B: if (ed_cx > 0) ed_cx--; else if (ed_cy > 0) {
                    ed_cy--; ed_cx = ed_line_len(ed_cy); } break;         /* left */
    case 0x14D: if (ed_cx < ed_line_len(ed_cy)) ed_cx++; else if (ed_cy + 1 < ed_lines()) {
                    ed_cy++; ed_cx = 0; } break;                          /* right */
    case 0x147: ed_cx = 0; break;                                        /* home */
    case 0x14F: ed_cx = ed_line_len(ed_cy); break;                       /* end */
    case 0x153: ed_delete(); break;                                      /* del */
    case 0x202: ed_save(); break;                                        /* F2 */
    case 0x203: ed_refresh_picker(); ed_pick = 1; break;                 /* F3 */
    case 0x204: ed_new(); break;                                         /* F4 */
    case '\b': case 127: ed_backspace(); break;
    case '\n': case '\r': ed_insert('\n'); break;
    case '\t': for (int i = 0; i < 4; i++) ed_insert(' '); break;
    default:
        if (c >= 32 && c < 127) ed_insert((char)c);
        break;
    }
}

static void calc_key(int c) {
    if (c == '=' || c == '\r' || c == '\n') {
        int r, rc = calc_eval(calc_buf, &r);
        if (rc == 1) { calc_val = r; calc_ok = 1; calc_err[0] = '\0'; }
        else { calc_ok = 0; strncpy(calc_err, rc == 0 ? "division by zero"
                                                      : "syntax error",
                                    sizeof calc_err - 1);
               calc_err[sizeof calc_err - 1] = '\0'; }
    } else if (c == 'c' || c == 'C' || c == 27) {
        calc_len = 0; calc_buf[0] = '\0'; calc_ok = 0; calc_err[0] = '\0';
    } else if (c == '\b' || c == 127) {
        if (calc_len) calc_buf[--calc_len] = '\0';
        calc_ok = 0; calc_err[0] = '\0';
    } else if ((c >= '0' && c <= '9') || c == '+' || c == '-' ||
               c == '*' || c == '/') {
        if (calc_len < (int)sizeof calc_buf - 1) {
            calc_buf[calc_len++] = (char)c;
            calc_buf[calc_len] = '\0';
        }
        calc_ok = 0; calc_err[0] = '\0';
    }
}

void gui_enter(void) {
    if (!gfx_is_vbe()) {
        /* no VBE: keep the shell usable instead of scribbling mode 13h */
        serial_puts("[gui] no VBE framebuffer - staying in text mode\n");
        gfx_clear(0);
        return;
    }
    W = gfx_screen_w();
    H = gfx_screen_h();
    SC = 2;
    SM = 1;
    if (H >= 1080) SC = 2;
    if (H >= 1600) SC = 3;

    /* the mouse driver still clamped to 320x200: give it the real size and
     * a motion multiplier so the pointer can cross a 1024px screen */
    mouse_set_bounds(W, H, W / 320 > 1 ? W / 320 : 1);

    for (int i = 0; i < MAX_WIN; i++) wins[i].used = 0;
    focus = -1;

    /* a fresh session starts with the editor and the file browser open -
     * that is what people expect to see on a desktop */
    ed_refresh_picker();
    win_open(APP_EDITOR);
    win_open(APP_FILES);

    render_all();
    cursor_draw(mouse_pos_x(), mouse_pos_y());

    serial_puts("[gui] NovaOS 1.0 desktop up\n");

    int drag = -1, drag_dx = 0, drag_dy = 0;
    int need_full = 0;
    int last_mx = -1, last_my = -1;

    for (;;) {
        int click = mouse_left_clicked();
        int rel   = mouse_left_released();
        int down  = mouse_left_down();
        int mx    = mouse_pos_x();
        int my    = mouse_pos_y();

        /* ---- keyboard (drain a few per frame) ---- */
        int key, nk = 0;
        while ((key = keyboard_getc()) != 0 && nk < 8) {
            nk++;
            if (key == 0x201) {                       /* F1: editor */
                win_open(APP_EDITOR); need_full = 1; continue;
            }
            if (key == 27) {                          /* Esc closes */
                if (focus >= 0) { win_close(focus); need_full = 1; }
                continue;
            }
            if (focus < 0) continue;
            switch (wins[focus].app) {
            case APP_EDITOR: editor_key(key); break;
            case APP_CALC:   calc_key(key);   break;
            default: break;
            }
            need_full = 1;
        }

        /* ---- mouse ---- */
        if (click) {
            int id = btn_hit(mx, my);
            if (id == -1 && my < TOPH) {
                /* top bar: nothing to do yet */
            } else if (id >= 900 && id < 900 + APP_COUNT) {
                win_open(id - 900);
                need_full = 1;
            } else if (id >= 100 && id < 110 && focus >= 0 &&
                       wins[focus].app == APP_EDITOR) {
                if (id == 100) ed_save();
                else if (id == 101) { ed_refresh_picker(); ed_pick = 1; }
                else if (id == 102) ed_new();
                need_full = 1;
            } else if (id >= 200 && id < 220 && focus >= 0 &&
                       wins[focus].app == APP_EDITOR) {
                int i = id - 200;
                if (i < pk_n) ed_load(pk_names[i]);
                need_full = 1;
            } else if (id >= 300 && id < 320 && focus >= 0 &&
                       wins[focus].app == APP_FILES) {
                int i = id - 300;
                if (i < fl_n) {
                    ed_load(fl_names[i]);
                    win_open(APP_EDITOR);
                }
                need_full = 1;
            } else if (id >= 400 && id < 420 && focus >= 0 &&
                       wins[focus].app == APP_CALC) {
                static const char *keys[16] = {
                    "7","8","9","/", "4","5","6","*", "1","2","3","-",
                    "0",".","=","+"
                };
                calc_key(keys[id - 400][0]);
                need_full = 1;
            } else if (id >= 500 && id < 510 && focus >= 0 &&
                       wins[focus].app == APP_SETTINGS) {
                u32 cols[3] = { RGB(59,130,246), RGB(233,84,32), RGB(16,185,129) };
                accent = cols[id - 500];
                accent_dim = darken(accent, 150);
                need_full = 1;
            } else {
                int wi = win_at(mx, my);
                if (wi >= 0) {
                    win_raise(wi);
                    /* traffic-light close */
                    int cy = wins[wi].y + TH / 2, r = PX(9);
                    if (mx >= wins[wi].x + PX(16) - r && mx <= wins[wi].x + PX(16) + r &&
                        my >= cy - r && my <= cy + r) {
                        win_close(wi);
                        need_full = 1;
                    } else if (my <= wins[wi].y + TH) {
                        drag = wi;
                        drag_dx = mx - wins[wi].x;
                        drag_dy = my - wins[wi].y;
                    } else if (wins[wi].app == APP_EDITOR) {
                        editor_click(&wins[wi], mx, my);
                    } else if (wins[wi].app == APP_FILES) {
                        files_refresh();
                    }
                    need_full = 1;
                }
            }
        }
        if (rel) drag = -1;

        if (drag >= 0 && down) {
            int nx = mx - drag_dx, ny = my - drag_dy;
            if (nx < -wins[drag].w / 2) nx = -wins[drag].w / 2;
            if (ny < TOPH) ny = TOPH;
            if (nx > W - PX(80)) nx = W - PX(80);
            if (ny > H - DOCKH - PX(30)) ny = H - DOCKH - PX(30);
            if (nx != wins[drag].x || ny != wins[drag].y) {
                wins[drag].x = nx;
                wins[drag].y = ny;
                need_full = 1;
            }
        }

        if (need_full) {
            cursor_restore();
            render_all();
            cursor_draw(mx, my);
            need_full = 0;
            last_mx = mx; last_my = my;
        } else if (mx != last_mx || my != last_my) {
            cursor_restore();
            cursor_draw(mx, my);
            last_mx = mx; last_my = my;
        }

        hlt();
    }
}
