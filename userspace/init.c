/* NovaOS - userland program example.
 *
 * This file demonstrates what a NovaOS user-space process looks like:
 * it is compiled as a standalone flat binary (no kernel linkage) and is
 * expected to be loaded by a future ELF loader and run in ring 3.
 *
 * It only uses the int 0x80 syscall ABI:
 *     eax = 0  sys_write(fd, buf, len)     (ebx, ecx, edx)
 *     eax = 2  sys_exit(code)              (ebx)
 *
 * Status: the syscall ABI exists in the kernel (kernel/syscall.c) and is
 * exercised by kernel threads; a ring-3 loader + ELF support is on the
 * roadmap (docs/ROADMAP.md, milestone 0.5).
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

#define SYS_WRITE 0
#define SYS_EXIT  2

void _start(void) {
    const char *msg = "Hello from NovaOS userland (pid via sys_getpid soon)\n";
    syscall3(SYS_WRITE, 1, (u32)msg, 40);
    syscall3(SYS_EXIT, 0, 0, 0);
    for (;;) {}
}
