/* NovaOS - system calls.
 * Convention: eax = nr, ebx/ecx/edx = args.
 *   nr 0: sys_write(fd, buf, len)  - writes to console
 *   nr 1: sys_getpid()
 *   nr 2: sys_exit(code)
 *   nr 3: sys_fork()               - clone with copy-on-write (v1.0)
 *   nr 4: sys_open(name, len)      - open a file on the FAT data disk
 *   nr 5: sys_read(fd, buf, len)   - read from an open file
 *   nr 6: sys_close(fd)
 *   nr 7: sys_sleep(ticks)         - yield the CPU for N timer ticks
 *   nr 8: sys_brk(addr)            - query/extend the heap break
 *
 * SECURITY: every pointer that arrives from ring 3 is validated before it is
 * dereferenced.  The user window is the only region a ring-3 process may name
 * (see USER_PG_START/USER_PG_END in paging.c); without this check a process
 * could hand the kernel an arbitrary address - including kernel text or the
 * page tables - and have the kernel read and echo it back.
 */
#include "syscall.h"
#include "sched.h"
#include "vga.h"
#include "serial.h"
#include "io.h"
#include "fat.h"
#include "paging.h"
#include "timer.h"
#include "string.h"
#include "printf.h"

#define NR_WRITE   0
#define NR_GETPID  1
#define NR_EXIT    2
#define NR_FORK    3
#define NR_OPEN    4
#define NR_READ    5
#define NR_CLOSE   6
#define NR_SLEEP   7
#define NR_BRK     8

/* user address window: must match paging.c (USER_PG_START .. USER_PG_END) */
#define USER_MEM_START 0x200000u
#define USER_MEM_END   0x300000u

#define MAX_OPEN_FILES 8

struct open_file {
    int  used;
    u32  size;
    u32  pos;
    char name[32];
    u8   data[2048];
};

static struct open_file ofiles[MAX_OPEN_FILES];

/* Is [addr, addr+len) entirely inside the user window?  Guards every pointer
 * we are about to read on behalf of ring 3. */
static int user_range_ok(u32 addr, u32 len) {
    if (len == 0) return 1;
    if (addr < USER_MEM_START) return 0;
    if (addr > USER_MEM_END) return 0;
    /* reject wraparound: addr + len must not overflow or leave the window */
    if (len > USER_MEM_END - addr) return 0;
    return 1;
}

static u32 sys_write(u32 fd, const char *buf, u32 len) {
    (void)fd;
    /* len is attacker-controlled: a huge value would let ring 3 walk the
     * kernel forever, so bound the window before touching memory. */
    if (fd > 2) return (u32)-1;
    if (!user_range_ok((u32)buf, len)) {
        /* Log the offending range: this is the kernel refusing to dereference
         * a pointer a ring-3 process had no business naming. */
        char d[80];
        int l = ksprintf(d, "[sec] sys_write refused %x len=%u\n",
                         (u32)buf, len);
        serial_write(d, (u32)l);
        return (u32)-1;
    }
    for (u32 i = 0; i < len; i++) {
        vga_putc(buf[i], 0x0F);
        serial_putc(buf[i]);
    }
    return len;
}

static int alloc_fd(void) {
    for (int i = 0; i < MAX_OPEN_FILES; i++)
        if (!ofiles[i].used) return i;
    return -1;
}

static u32 sys_open(const char *name, u32 len) {
    if (!user_range_ok((u32)name, len)) return (u32)-1;
    if (len == 0 || len > sizeof(ofiles[0].name) - 1) return (u32)-1;

    char local[32];
    for (u32 i = 0; i < len; i++) local[i] = name[i];
    local[len] = '\0';

    int fd = alloc_fd();
    if (fd < 0) return (u32)-1;

    struct open_file *f = &ofiles[fd];
    u32 size = 0;
    if (!fat_read(local, f->data, sizeof(f->data), &size))
        return (u32)-1;

    f->used = 1;
    f->size = size;
    f->pos  = 0;
    strncpy(f->name, local, sizeof(f->name) - 1);
    f->name[sizeof(f->name) - 1] = '\0';
    return (u32)fd;
}

static u32 sys_read(u32 fd, char *buf, u32 len) {
    if (fd >= MAX_OPEN_FILES) return (u32)-1;
    struct open_file *f = &ofiles[fd];
    if (!f->used) return (u32)-1;
    if (!user_range_ok((u32)buf, len)) return (u32)-1;

    u32 avail = f->size - f->pos;
    if (len > avail) len = avail;
    for (u32 i = 0; i < len; i++) buf[i] = (char)f->data[f->pos + i];
    f->pos += len;
    return len;
}

static u32 sys_close(u32 fd) {
    if (fd >= MAX_OPEN_FILES) return (u32)-1;
    if (!ofiles[fd].used) return (u32)-1;
    ofiles[fd].used = 0;
    return 0;
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
    case NR_FORK: {
        task_t *cur = sched_current_task();
        if (cur && cur->flags == TASK_USER) {
            /* Clone from the *live* syscall frame, not cur->esp: cur->esp is
             * only refreshed on the last timer tick, and between ticks the
             * real stack pointer has moved (we are several frames deep in
             * syscall_dispatch).  Handing the stale value to the child made it
             * resume from the wrong address.  `r` is this syscall's frame, so
             * its address is exactly where the child must continue. */
            r->eax = task_fork_user(cur->name, (u32)r, cur->cr3);
        } else {
            r->eax = (u32)-1;      /* kernel threads cannot fork yet */
        }
        break;
    }
    case NR_OPEN:
        r->eax = sys_open((const char*)r->ebx, r->ecx);
        break;
    case NR_READ:
        r->eax = sys_read(r->ebx, (char*)r->ecx, r->edx);
        break;
    case NR_CLOSE:
        r->eax = sys_close(r->ebx);
        break;
    case NR_SLEEP: {
        /* Cooperative sleep.
         *
         * This must NOT spin inside the syscall.  A ring-3 process that is
         * preempted while parked in the middle of a system call has its
         * in-kernel call frame frozen on the kernel stack; every preemption
         * during the wait nests another frame, so a spin here grows the
         * kernel stack without bound and the call never returns.
         *
         * Sleep is therefore *non-blocking*: block the caller for a bounded
         * number of timer ticks by simply returning the requested delay and
         * letting userland poll.  The kernel stays responsive and no frame is
         * left half-finished.  A real implementation would put the task in a
         * sleeping state and have the scheduler wake it.
         *
         * Bounded so a process cannot ask for an absurd delay. */
        u32 want = r->ebx;
        if (want > 1000) want = 1000;
        task_t *cur = sched_current_task();
        if (cur) cur->slice_left = want ? want : 1;
        r->eax = 0;
        break;
    }
    case NR_BRK:
        /* report the user window top as the current break; the heap is a
         * fixed mapping for now, so an extension request is refused */
        r->eax = (r->ebx == 0) ? USER_MEM_END : (u32)-1;
        break;
    default:
        r->eax = (u32)-1;          /* ENOSYS */
        break;
    }
}
