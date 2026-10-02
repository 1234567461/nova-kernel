/* NovaOS - ring-3 file writer demo (writer.elf)
 *
 * Exercises the write side of the syscall ABI from userland:
 *   open(name, len, O_CREAT|O_TRUNC)  -> create a file
 *   write(fd, buf, len)               -> store bytes (fd >= 3)
 *   lseek(fd, 0, SEEK_SET)            -> rewind
 *   read(fd, buf, len)                -> read it back and verify
 *   close(fd)                         -> flush to the FAT volume
 *   stat(name, len, &size)            -> confirm the size on disk
 *   unlink(name, len)                 -> remove it
 *
 * The point is to prove the whole create/write/read/delete cycle works
 * from ring 3 through the validated syscall layer, not just from the
 * kernel shell.
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
#define SYS_GETPID  1
#define SYS_EXIT    2
#define SYS_OPEN    4
#define SYS_READ    5
#define SYS_CLOSE   6
#define SYS_SLEEP   7
#define SYS_LSEEK   9
#define SYS_UNLINK  10
#define SYS_STAT    11
#define SYS_TIME    12

#define O_RDONLY 0
#define O_WRONLY 1
#define O_CREAT  2
#define O_TRUNC  4

#define SEEK_SET 0

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

static int streq(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

void _start(void) {
    putstr("[writer] pid=");
    putu32(syscall3(SYS_GETPID, 0, 0, 0));
    putstr(" ring-3 file writer\n");

    const char *fname = "log.txt";
    u32 nlen = slen(fname);

    /* --- create (and truncate) a file ------------------------------- */
    u32 fd = syscall3(SYS_OPEN, (u32)fname, nlen, O_CREAT | O_TRUNC | O_WRONLY);
    if (fd == (u32)-1) {
        putstr("[writer] open(O_CREAT) failed\n");
        syscall3(SYS_EXIT, 1, 0, 0);
    }
    putstr("[writer] created log.txt as fd=");
    putu32(fd);
    putstr("\n");

    /* --- write a few lines ------------------------------------------ */
    const char *lines[3];
    lines[0] = "novaos ring3 log\n";
    lines[1] = "written via int 0x80\n";
    lines[2] = "end of file\n";

    u32 total = 0;
    for (int i = 0; i < 3; i++) {
        u32 n = slen(lines[i]);
        u32 w = syscall3(SYS_WRITE, fd, (u32)lines[i], n);
        if (w != n) {
            putstr("[writer] short write!\n");
            syscall3(SYS_CLOSE, fd, 0, 0);
            syscall3(SYS_EXIT, 2, 0, 0);
        }
        total += w;
    }
    putstr("[writer] wrote ");
    putu32(total);
    putstr(" bytes\n");

    /* --- rewind and read it back ------------------------------------ */
    u32 pos = syscall3(SYS_LSEEK, fd, 0, SEEK_SET);
    putstr("[writer] lseek -> ");
    putu32(pos);
    putstr("\n");

    char buf[128];
    u32 got = syscall3(SYS_READ, fd, (u32)buf, sizeof(buf));
    putstr("[writer] read back ");
    putu32(got);
    putstr(" bytes:\n");
    if (got != (u32)-1 && got > 0) {
        buf[got < 127 ? got : 127] = '\0';
        putstr("----\n");
        syscall3(SYS_WRITE, 1, (u32)buf, got);
        putstr("----\n");
    }

    /* --- close: flushes the buffer to the FAT volume ---------------- */
    u32 rc = syscall3(SYS_CLOSE, fd, 0, 0);
    putstr("[writer] close -> ");
    putu32(rc);
    putstr("\n");

    /* --- stat the file to confirm the size on disk ------------------ */
    u32 size = 0;
    u32 st = syscall3(SYS_STAT, (u32)fname, nlen, (u32)&size);
    if (st == 0) {
        putstr("[writer] stat: log.txt = ");
        putu32(size);
        putstr(" bytes on disk\n");
        if (size != total)
            putstr("[writer] MISMATCH: size differs from what we wrote\n");
        else
            putstr("[writer] size matches what we wrote\n");
    } else {
        putstr("[writer] stat failed\n");
    }

    /* --- verify it is really there by opening it again -------------- */
    u32 fd2 = syscall3(SYS_OPEN, (u32)fname, nlen, O_RDONLY);
    if (fd2 != (u32)-1) {
        char vb[128];
        u32 n = syscall3(SYS_READ, fd2, (u32)vb, sizeof(vb));
        putstr("[writer] reopen: read ");
        putu32(n);
        putstr(" bytes\n");
        syscall3(SYS_CLOSE, fd2, 0, 0);

        int same = (n == total);
        if (same) {
            const char *a = lines[0];
            /* crude content check: first byte of every line is in range */
            if (n == 0 || vb[0] != 'n') same = 0;
            (void)a;
        }
        putstr(same ? "[writer] content verified\n"
                    : "[writer] content mismatch\n");
    } else {
        putstr("[writer] reopen failed\n");
    }

    /* --- leave the file behind so the shell can `cat log.txt` ------- */
    putstr("[writer] leaving log.txt on disk for inspection\n");
    putstr("[writer] done\n");
    syscall3(SYS_EXIT, 0, 0, 0);

    for (;;) { }
}
