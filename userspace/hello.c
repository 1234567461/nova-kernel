/* NovaOS - ring-3 userland demo program.
 *
 * Compiled as a static ELF32, linked at 0x200000 (USER_TEXT_BASE) and
 * entered at _start in ring 3.  It talks to the kernel only through the
 * int 0x80 ABI:  0=write(fd,buf,len) 1=getpid() 2=exit(code).
 * No libc, no hardware access - memory beyond the user window is
 * supervisor-only and would page-fault.
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
    putstr("[ring3] hello from userland, pid=");
    putu32(syscall3(SYS_GETPID, 0, 0, 0));
    putstr("\n");
    putstr("[ring3] running on the NovaOS kernel (int 0x80 ABI)\n");
    putstr("[ring3] user page flags: U/S set, kernel memory is protected\n");
    putstr("[ring3] exiting cleanly\n");
    syscall3(SYS_EXIT, 0, 0, 0);
    for (;;) { }                       /* unreachable */
}
