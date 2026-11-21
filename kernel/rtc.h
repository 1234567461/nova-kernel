#ifndef NOVA_RTC_H
#define NOVA_RTC_H
#include "common.h"

typedef struct {
    u32  year;
    u8   mon, day;
    u8   hour, min, sec;
    const char *wd;         /* "Mon".."Sun" (computed, not read from CMOS) */
} rtc_time;

int rtc_get(rtc_time *t);       /* 1 = ok, 0 = firmware clock garbage */
int rtc_str(char *buf, u32 n);  /* "Fri 2026-11-21 12:34:56 RTC", 0 = failed */

#endif
