/* NovaOS - system calls.
 * Convention: eax = nr, ebx/ecx/edx = args.
 *   nr 0: sys_write(fd, buf, len)  - writes to console
 *   nr 1: sys_getpid()
 *   nr 2: sys_exit(code)
 */
#include "syscall.h"
#include "sched.h"
#include "vga.h"
#include "serial.h"
#include "io.h"

#define NR_WRITE   0
#define NR_GETPID  1
#define NR_EXIT    2

static u32 sys_write(u32 fd, const char *buf, u32 len) {
    (void)fd;
    for (u32 i = 0; i < len; i++) {
        vga_putc(buf[i], 0x0F);
        serial_putc(buf[i]);
    }
    return len;
}

void syscall_dispatch(struct regs *r) {
    switch (r->eax) {
    case NR_WRITE:
        r->eax = sys_write(r->ebx, (const char*)r->ecx, r->edx);
        break;
    case NR_GETPID:
        r->eax = sched_current_pid();
        break;
    case NR_EXIT:
        sched_exit_current();      /* switches away, never returns */
        break;
    default:
        r->eax = (u32)-1;          /* ENOSYS */
        break;
    }
}
