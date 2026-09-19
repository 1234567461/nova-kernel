/* NovaOS - PIT timer driver and tick counter */
#ifndef NOVA_TIMER_H
#define NOVA_TIMER_H

#include "common.h"

void timer_init(u32 hz);
void timer_wait(u32 ticks);
u32  timer_ticks(void);

#endif
