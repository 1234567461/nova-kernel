/* NovaOS - boot log implementation.
 * Timestamps come from the PIT (100 Hz), so steps after timer_init() show
 * real elapsed time; everything before it prints [ 0.000s]. */
#include "bootlog.h"
#include "common.h"
#include "vga.h"
#include "serial.h"
#include "timer.h"
#include "printf.h"
#include "string.h"

#define STEP_COL   12          /* column where the filler dots end */
#define NAME_MAX   24

/* "[  0.012s] paging: per-process tables " = 11 + up to NAME_MAX chars */
static char step_line[16 + NAME_MAX + 2];
static int  step_active = 0;

static void emit_text(const char *s) {
    serial_write(s, (u32)strlen(s));
}

static void emit_colored(const char *s, u8 attr) {
    vga_write(s, attr);
    emit_text(s);
}

static void timestamp(char out[12]) {
    u32 t = timer_ticks();                 /* 100 Hz */
    ksprintf(out, "[%2u.%02us]", t / 100, t % 100);
}

void bootlog_banner(void) {
    emit_colored("==================================================\n", 0x08);
    emit_colored("  NovaOS v0.9 Aurora building - the home-grown operating system\n", 0x0F);
    emit_colored("  stage1 -> stage2 -> pmode -> paging -> ring3\n", 0x08);
    emit_colored("==================================================\n", 0x08);
    emit_text("\n");
}

void bootlog_start(const char *step, const char *detail) {
    char ts[12];
    timestamp(ts);

    char left[NAME_MAX + 4];
    if (detail && detail[0])
        ksprintf(left, "%s: %s", step, detail);
    else
        ksprintf(left, "%s", step);

    /* truncate the label so the filler column stays aligned */
    u32 ll = strlen(left);
    if (ll > NAME_MAX) ll = NAME_MAX;

    u32 pos = 0;
    ksprintf(step_line, "%s ", ts);
    pos = strlen(step_line);
    memcpy(step_line + pos, left, ll);
    pos += ll;
    for (u32 i = ll; i < STEP_COL; i++)
        step_line[pos++] = '.';
    step_line[pos++] = ' ';
    step_line[pos] = '\0';

    emit_colored(step_line, 0x07);
    step_active = 1;
}

void bootlog_ok(const char *note) {
    if (!step_active) return;
    step_active = 0;
    if (note && note[0]) {
        emit_colored("ok (", 0x0A);
        emit_colored(note, 0x0A);
        emit_colored(")\n", 0x0A);
    } else {
        emit_colored("ok\n", 0x0A);
    }
}

void bootlog_fail(const char *note) {
    if (!step_active) return;
    step_active = 0;
    if (note && note[0]) {
        emit_colored("FAIL (", 0x0C);
        emit_colored(note, 0x0C);
        emit_colored(")\n", 0x0C);
    } else {
        emit_colored("FAIL\n", 0x0C);
    }
}

void bootlog_done(void) {
    u32 t = timer_ticks();
    char buf[48];
    ksprintf(buf, "\nboot complete in %u.%02u s\n\n", t / 100, t % 100);
    emit_text(buf);
    emit_colored("NovaOS shell - type 'help' for commands\n", 0x0B);
    emit_text("\n");
}
