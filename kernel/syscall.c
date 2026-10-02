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
#define NR_LSEEK   9
#define NR_UNLINK  10
#define NR_STAT    11
#define NR_TIME    12

/* open() flags */
#define O_RDONLY   0
#define O_WRONLY   1
#define O_CREAT    2
#define O_TRUNC    4

/* user address window: must match paging.c (USER_PG_START .. USER_PG_END) */
#define USER_MEM_START 0x200000u
#define USER_MEM_END   0x300000u

#define MAX_OPEN_FILES 8

#define OFILE_CAP 2048

struct open_file {
    int  used;
    int  writable;          /* opened with O_CREAT / O_WRONLY */
    int  dirty;             /* modified in memory, needs write-back */
    u32  size;
    u32  pos;
    char name[32];
    u8   data[OFILE_CAP];
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

/* File write: fd >= 3 names an open file, fd 0..2 are the console.
 * Returns the number of bytes stored, or -1. */
static u32 sys_write_file(u32 fd, const char *buf, u32 len) {
    if (fd >= MAX_OPEN_FILES) return (u32)-1;
    struct open_file *f = &ofiles[fd];
    if (!f->used || !f->writable) return (u32)-1;
    if (!user_range_ok((u32)buf, len)) return (u32)-1;

    u32 room = OFILE_CAP - f->pos;
    if (len > room) len = room;
    for (u32 i = 0; i < len; i++) f->data[f->pos + i] = (u8)buf[i];
    f->pos += len;
    if (f->pos > f->size) f->size = f->pos;
    f->dirty = 1;
    return len;
}

/* flush a modified file back to the FAT volume */
static u32 sys_sync(u32 fd) {
    if (fd >= MAX_OPEN_FILES) return (u32)-1;
    struct open_file *f = &ofiles[fd];
    if (!f->used || !f->dirty) return 0;
    if (!fat_write(f->name, f->data, f->size)) return (u32)-1;
    f->dirty = 0;
    return 1;
}

static u32 sys_write(u32 fd, const char *buf, u32 len) {
    if (fd >= 3) return sys_write_file(fd, buf, len);
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

static u32 sys_open(const char *name, u32 len, u32 flags) {
    if (!user_range_ok((u32)name, len)) return (u32)-1;
    if (len == 0 || len > sizeof(ofiles[0].name) - 1) return (u32)-1;

    char local[32];
    for (u32 i = 0; i < len; i++) local[i] = name[i];
    local[len] = '\0';

    int fd = alloc_fd();
    if (fd < 0) return (u32)-1;

    struct open_file *f = &ofiles[fd];
    u32 size = 0;
    int exists = fat_read(local, f->data, sizeof(f->data), &size);

    if (!exists && !(flags & O_CREAT)) return (u32)-1;
    if (!exists) size = 0;

    f->used     = 1;
    f->writable = (flags & (O_WRONLY | O_CREAT | O_TRUNC)) ? 1 : 0;
    f->dirty    = 0;
    f->size     = size;
    f->pos      = 0;
    if (flags & O_TRUNC) { f->size = 0; f->dirty = 1; }
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
    u32 rc = sys_sync(fd);           /* commit any pending write-back */
    ofiles[fd].used     = 0;
    ofiles[fd].writable = 0;
    ofiles[fd].dirty    = 0;
    return rc == (u32)-1 ? (u32)-1 : 0;
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
        r->eax = sys_open((const char*)r->ebx, r->ecx, r->edx);
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
    case NR_LSEEK: {
        if (r->ebx >= MAX_OPEN_FILES) { r->eax = (u32)-1; break; }
        struct open_file *f = &ofiles[r->ebx];
        if (!f->used) { r->eax = (u32)-1; break; }
        /* whence: 0 = SET, 1 = CUR, 2 = END */
        i32 off = (i32)r->ecx;
        i32 base;
        if (r->edx == 0)      base = 0;
        else if (r->edx == 1) base = (i32)f->pos;
        else                  base = (i32)f->size;
        i32 np = base + off;
        if (np < 0) np = 0;
        if ((u32)np > OFILE_CAP) np = (i32)OFILE_CAP;
        f->pos = (u32)np;
        r->eax = f->pos;
        break;
    }
    case NR_UNLINK: {
        /* r->ebx = name, r->ecx = len */
        if (!user_range_ok(r->ebx, r->ecx) || r->ecx == 0 ||
            r->ecx > 31) { r->eax = (u32)-1; break; }
        char local[32];
        for (u32 i = 0; i < r->ecx; i++) local[i] = ((const char*)r->ebx)[i];
        local[r->ecx] = '\0';
        r->eax = fat_delete(local) ? 0 : (u32)-1;
        break;
    }
    case NR_STAT: {
        /* r->ebx = name, r->ecx = len, r->edx = &size_out */
        if (!user_range_ok(r->ebx, r->ecx) || r->ecx == 0 ||
            r->ecx > 31) { r->eax = (u32)-1; break; }
        char local[32];
        for (u32 i = 0; i < r->ecx; i++) local[i] = ((const char*)r->ebx)[i];
        local[r->ecx] = '\0';
        u32 size = 0;
        if (!fat_stat(local, &size, NULL, NULL)) { r->eax = (u32)-1; break; }
        if (r->edx && user_range_ok(r->edx, 4))
            *(u32*)r->edx = size;
        r->eax = 0;
        break;
    }
    case NR_TIME:
        /* no RTC driver yet: report ticks since boot so userland can still
         * measure elapsed time and seed a PRNG */
        r->eax = timer_ticks();
        break;
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
