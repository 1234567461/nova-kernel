/* NovaOS - ring-3 regression test: dirty-file survival (leaky.elf)
 *
 * Regression test for the exit-time flush fix.  The program opens a file,
 * writes to it, and exits WITHOUT ever calling close().  Before the fix
 * the dirty data sat in the kernel's global open-file table forever: the
 * FAT never saw it, and cat leaky.txt after the run showed an empty or
 * missing file.  With files_flush_all() in sched_exit_current() the bytes
 * must be on disk by the time the shell regains the prompt.
 *
 * A second block exercises the sys_read bounds fix: lseek() past EOF and
 * try to read - the kernel must return 0 bytes (EOF), not data read out
 * of bounds past the file buffer.
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

#define SYS_WRITE   0
#define SYS_EXIT    2
#define SYS_OPEN    4
#define SYS_READ    5
#define SYS_LSEEK   9

#define O_WRONLY 1
#define O_CREAT  2
#define O_TRUNC  4
#define SEEK_SET 0
#define SEEK_END 2

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

void _start(void) {
    /* --- 1. dirty file that is never closed -------------------------- */
    const char *fname = "leaky.txt";
    u32 fd = syscall3(SYS_OPEN, (u32)fname, slen(fname),
                      O_CREAT | O_TRUNC | O_WRONLY);
    if (fd == (u32)-1) {
        putstr("[leaky] open failed\n");
        syscall3(SYS_EXIT, 1, 0, 0);
    }
    putstr("[leaky] opened leaky.txt as fd=");
    putu32(fd);
    putstr("\n");

    const char *msg = "leaky file data survived exit without close";
    u32 w = syscall3(SYS_WRITE, fd, (u32)msg, slen(msg));
    putstr("[leaky] wrote ");
    putu32(w);
    putstr(" bytes, NOT closing\n");

    /* --- 2. read past EOF must return 0, not leak ------------------- */
    u32 rfd = syscall3(SYS_OPEN, (u32)fname, slen(fname), 0 /* O_RDONLY */);
    if (rfd != (u32)-1) {
        /* seek far past the end of this small file */
        u32 pos = syscall3(SYS_LSEEK, rfd, 3000, SEEK_SET);
        char sink[64];
        u32 got = syscall3(SYS_READ, rfd, (u32)sink, sizeof(sink));
        putstr("[leaky] seek->");
        putu32(pos);
        putstr(" read-past-EOF got ");
        putu32(got);
        putstr(got == 0 ? " bytes (ok)\n" : " bytes (LEAK!)\n");
    }

    /* --- exit with the first descriptor still open and dirty -------- */
    putstr("[leaky] exiting without close()\n");
    syscall3(SYS_EXIT, 0, 0, 0);
    for (;;) { }
}
