/* NovaOS - PIT timer (IRQ0) */
#include "timer.h"
#include "io.h"
#include "idt.h"
#include "irq.h"

static volatile u32 ticks = 0;
static u32 freq = 100;

static void timer_handler(struct regs *r) {
    (void)r;
    ticks++;
}

void timer_init(u32 hz) {
    freq = hz;
    u32 divisor = 1193180 / hz;
    outb(0x43, 0x36);
    outb(0x40, (u8)(divisor & 0xFF));
    outb(0x40, (u8)((divisor >> 8) & 0xFF));
    register_irq_handler(0, timer_handler);
    irq_enable(0);
}

void timer_wait(u32 t) {
    u32 target = ticks + t;
    while (ticks < target) { hlt(); }
}

u32 timer_ticks(void) { return ticks; }
