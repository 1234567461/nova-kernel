/* NovaOS - PS/2 mouse driver.
 * Uses the 8042 controller: enables the AUX channel and IRQ12, then
 * reads 3-byte packets (flags, dx, dy) from port 0x60.
 */
#include "mouse.h"
#include "io.h"
#include "idt.h"
#include "irq.h"

#define MOUSE_IRQ 12
#define MOUSE_MAX 100000

static int  m_x = 160, m_y = 100;
static u8   m_buttons = 0, m_prev = 0;
static u8   m_pkt[3];
static int  m_cycle = 0;
static int  m_enabled = 0;

static void wait_out(void) {
    for (u32 i = 0; i < MOUSE_MAX; i++)
        if (inb(0x64) & 0x02) return;
}
static void wait_in(void) {
    for (u32 i = 0; i < MOUSE_MAX; i++)
        if (inb(0x64) & 0x01) return;
}
static void mouse_write(u8 b) {
    wait_out(); outb(0x64, 0xD4);
    wait_out(); outb(0x60, b);
}
static u8 mouse_read(void) {
    wait_in();
    return inb(0x60);
}

static void mouse_process(void) {
    int dx = (int)(i8)m_pkt[1];
    int dy = -(int)(i8)m_pkt[2];           /* screen Y grows down */
    m_x += dx;
    m_y += dy;
    if (m_x < 0) m_x = 0;
    if (m_x > 319) m_x = 319;
    if (m_y < 0) m_y = 0;
    if (m_y > 199) m_y = 199;
    m_buttons = (u8)(m_pkt[0] & 0x07);
}

static void mouse_handler(struct regs *r) {
    (void)r;
    if (!m_enabled) return;
    u8 d = inb(0x60);
    switch (m_cycle) {
    case 0: m_pkt[0] = d; m_cycle = 1; break;
    case 1: m_pkt[1] = d; m_cycle = 2; break;
    case 2: m_pkt[2] = d; m_cycle = 0; mouse_process(); break;
    default: m_cycle = 0; break;
    }
}

void mouse_init(void) {
    /* 1. enable the auxiliary device */
    wait_out(); outb(0x64, 0xA8);
    /* 2. enable IRQ12 in the 8042 command byte */
    wait_out(); outb(0x64, 0x20);
    wait_in(); u8 cmd = inb(0x60);
    cmd |= 0x02;
    wait_out(); outb(0x64, 0x60);
    wait_out(); outb(0x60, cmd);
    /* 3. set defaults + enable data reporting */
    mouse_write(0xF6); mouse_read();
    mouse_write(0xF4); mouse_read();

    register_irq_handler(MOUSE_IRQ, mouse_handler);
    irq_enable(MOUSE_IRQ);
    m_enabled = 1;
}

int mouse_pos_x(void) { return m_x; }
int mouse_pos_y(void) { return m_y; }
int mouse_left_down(void) { return (m_buttons & 1) ? 1 : 0; }

int mouse_left_clicked(void) {
    int c = (m_buttons & 1) && !(m_prev & 1);
    m_prev = m_buttons;
    return c;
}
int mouse_left_released(void) {
    int r = !(m_buttons & 1) && (m_prev & 1);
    m_prev = m_buttons;
    return r;
}
