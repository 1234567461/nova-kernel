/* NovaOS - ring-3 userland demo program (v1.0: fork + COW demo).
 *
 * Compiled as a static ELF32, linked at 0x200000 (USER_TEXT_BASE) and
 * entered at _start in ring 3.  It talks to the kernel only through the
 * int 0x80 ABI:
 *   0 = write(fd,buf,len)  1 = getpid()  2 = exit(code)  3 = fork()
 *
 * fork() returns the child pid to the parent and 0 to the child.
 * Both sides then write to the same .data page - that write trips the
 * copy-on-write handler (#PF), which copies the frame so each process
 * gets an independent copy.  The kernel prints COW events via
 * "meminfo" style counters; here we just demonstrate the semantics.
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

/* lives in .data: the first write after fork() forces a COW copy */
volatile u32 g_counter = 7;

static void putstr(const char *s) {
    u32 n = 0;
    while (s[n]) n++;
    syscall3(SYS_WRITE, 1, (u32)s, n);
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

void _start(void) {
    u32 me = syscall3(SYS_GETPID, 0, 0, 0);

    putstr("[ring3] pid=");
    putu32(me);
    putstr(" hello from userland\n");

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
        putstr("[ring3-parent] exiting\n");
        syscall3(SYS_EXIT, 0, 0, 0);
    }

    for (;;) { }                          /* unreachable */
}
