/* NovaOS - ring-3 userland program.
 *
 * Compiled as a static ELF32, linked at 0x200000 (USER_TEXT_BASE) and
 * entered at _start in ring 3.  It talks to the kernel only through the
 * int 0x80 ABI:
 *   0 = write(fd,buf,len)   1 = getpid()   2 = exit(code)   3 = fork()
 *   4 = open(name,len)      5 = read(fd,buf,len)            6 = close(fd)
 *   7 = sleep(ticks)        8 = brk(addr)
 *
 * Two things are demonstrated:
 *   1. file I/O - open/read/close pull a file off the FAT12 data disk
 *   2. fork + copy-on-write - both sides write to the same .data page and
 *      each ends up with an independent copy
 */
typedef unsigned int u32;

static inline u32 syscall3(u32 nr, u32 a, u32 b, u32 c) {
    u32 ret;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(nr), "b"(a), "c"(b), "d"(c)
        : "memory");
    return ret;
}

#define SYS_WRITE  0
#define SYS_GETPID 1
#define SYS_EXIT   2
#define SYS_FORK   3
#define SYS_OPEN   4
#define SYS_READ   5
#define SYS_CLOSE  6
#define SYS_SLEEP  7

/* lives in .data: the first write after fork() forces a COW copy */
volatile u32 g_counter = 7;

static u32 slen(const char *s) { u32 n = 0; while (s[n]) n++; return n; }

static void putstr(const char *s) {
    syscall3(SYS_WRITE, 1, (u32)s, slen(s));
}

static void putu32(u32 v) {
    char d[12];
    int i = 0;
    if (v == 0) d[i++] = '0';
    while (v) { d[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i > 0) {
        char c = d[--i];
        syscall3(SYS_WRITE, 1, (u32)&c, 1);
    }
}

/* Read a file from the FAT data disk and echo it, exercising the new
 * open/read/close syscalls. */
static void cat_file(const char *name) {
    u32 fd = syscall3(SYS_OPEN, (u32)name, slen(name), 0);
    if (fd == (u32)-1) {
        putstr("[ring3] open failed: ");
        putstr(name);
        putstr("\n");
        return;
    }
    putstr("[ring3] --- ");
    putstr(name);
    putstr(" ---\n");

    char buf[256];
    for (;;) {
        u32 n = syscall3(SYS_READ, fd, (u32)buf, sizeof(buf));
        if (n == (u32)-1) { putstr("[ring3] read error\n"); break; }
        if (n == 0) break;                       /* EOF */
        syscall3(SYS_WRITE, 1, (u32)buf, n);
    }
    syscall3(SYS_CLOSE, fd, 0, 0);
    putstr("\n[ring3] --- end ---\n");
}

void _start(void) {
    u32 me = syscall3(SYS_GETPID, 0, 0, 0);

    putstr("[ring3] pid=");
    putu32(me);
    putstr(" hello from userland\n");

    /* --- new: file I/O through the syscall ABI --- */
    cat_file("readme.txt");

    /* --- new: bounded cooperative sleep --- */
    putstr("[ring3] sleeping 5 ticks...\n");
    syscall3(SYS_SLEEP, 5, 0, 0);
    putstr("[ring3] awake again\n");
    /* --- original: fork + COW demo --- */
    putstr("[ring3] forking...\n");
    u32 child = syscall3(SYS_FORK, 0, 0, 0);

    if (child == 0) {
        /* ---- child: writes to the shared page -> COW copy ----------- */
        u32 my = syscall3(SYS_GETPID, 0, 0, 0);
        g_counter += 100;                 /* page fault -> copied */
        putstr("[ring3-child] pid=");
        putu32(my);
        putstr(" COW write ok, g_counter=");
        putu32(g_counter);
        putstr(" (independent copy)\n");
        putstr("[ring3-child] exiting\n");
        syscall3(SYS_EXIT, 0, 0, 0);
    } else {
        /* ---- parent: also writes -> its own COW copy ---------------- */
        g_counter += 1;
        putstr("[ring3-parent] pid=");
        putu32(me);
        putstr(" child pid=");
        putu32(child);
        putstr(" COW write ok, g_counter=");
        putu32(g_counter);
        putstr(" (independent copy)\n");
        /* Yield several times so the child gets a slice of the CPU and both
         * sides of the COW demo are actually observed.  Exiting immediately
         * here used to tear the parent's address space down before the child
         * ever ran, so only half the story was ever printed. */
        putstr("[ring3-parent] yielding to let the child run...\n");
        for (int i = 0; i < 30; i++)
            syscall3(SYS_SLEEP, 3, 0, 0);
        putstr("[ring3-parent] exiting\n");
        syscall3(SYS_EXIT, 0, 0, 0);
    }

    for (;;) { }                          /* unreachable */
}
