/* ============================================================================
 * NovaOS - real-time clock driver
 *
 * Reads the CMOS RTC (ports 0x70/0x71) for the machine's real wall-clock
 * time - the thing a "date" command is supposed to show.  Handles the two
 * traps every RTC reader must handle:
 *   - BCD-encoded values (default; register B bit 2 says binary)
 *   - 12-hour clock (register B bit 1; bit 7 of hours = PM)
 * and waits out the update-in-progress flag so a read never straddles the
 * second boundary.
 * ========================================================================== */
#include "common.h"
#include "io.h"
#include "printf.h"
#include "rtc.h"

#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

static u8 cmos_read(u8 reg) {
    outb(CMOS_ADDR, reg);
    return inb(CMOS_DATA);
}

static u8 bcd_to_bin(u8 v) { return (u8)((v & 0x0F) + ((v >> 4) * 10)); }

static int rtc_updating(void) {
    outb(CMOS_ADDR, 0x0A);
    return inb(CMOS_DATA) & 0x80;
}

static const char *const wd_name[7] = {
    "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
};

/* day-of-week via Zeller's congruence (RTC's register 6 is unreliable on
 * many firmwares, so we compute it ourselves) */
static u8 rtc_weekday(u32 y, u32 m, u32 d) {
    u32 k = d, c, yy;
    if (m < 3) { m += 12; y -= 1; }
    yy = y % 100;
    c  = y / 100;
    u32 w = k + 13 * (m + 1) / 5 + yy + yy / 4 + c / 4 + 5 * c;
    return (u8)(w % 7);      /* 0 = Saturday -> shift below */
}

int rtc_get(rtc_time *t) {
    /* spin off the update-in-progress flag (bounded - never hangs) */
    for (int i = 0; i < 500000 && rtc_updating(); i++)
        ;

    u8 sec  = cmos_read(0x00);
    u8 min  = cmos_read(0x02);
    u8 hour = cmos_read(0x04);
    u8 day  = cmos_read(0x07);
    u8 mon  = cmos_read(0x08);
    u8 year = cmos_read(0x09);
    u8 regb = cmos_read(0x0B);

    /* re-read once after UIP cleared: guards against straddling a tick */
    for (int i = 0; i < 500000 && rtc_updating(); i++)
        ;
    if (cmos_read(0x00) != sec) {
        sec  = cmos_read(0x00);
        min  = cmos_read(0x02);
        hour = cmos_read(0x04);
    }

    if (!(regb & 0x04)) {               /* BCD mode */
        sec  = bcd_to_bin(sec);
        min  = bcd_to_bin(min);
        hour = bcd_to_bin(hour & 0x7F);
        day  = bcd_to_bin(day);
        mon  = bcd_to_bin(mon);
        year = bcd_to_bin(year);
    }

    if (!(regb & 0x02)) {               /* 12-hour mode */
        u8 pm = cmos_read(0x04) & 0x80;
        if (pm && hour < 12) hour += 12;
        else if (!pm && hour == 12) hour = 0;
    }

    if (sec > 59 || min > 59 || hour > 23 || mon < 1 || mon > 12 ||
        day < 1 || day > 31)
        return 0;                        /* firmware clock is garbage */

    t->sec  = sec;
    t->min  = min;
    t->hour = hour;
    t->day  = day;
    t->mon  = mon;
    t->year = 2000 + year;               /* assume 20xx; fine until 2099 */
    t->wd   = wd_name[rtc_weekday(t->year, mon, day)];
    return 1;
}

int rtc_str(char *buf, u32 n) {
    rtc_time t;
    if (!rtc_get(&t)) return 0;
    return ksprintf(buf, "%s %u-%02u-%02u %02u:%02u:%02u RTC",
                    t.wd, t.year, t.mon, t.day, t.hour, t.min, t.sec) < (int)n;
}
