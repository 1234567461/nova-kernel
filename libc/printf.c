/* NovaOS libc - minimal formatting engine.
 * Supports: %c %s %d %u %x %X %p %%. No floats, no width modifiers
 * (kept deliberately small; width/float parsing is on the roadmap).
 */
#include "printf.h"
#include "string.h"
#include "../kernel/serial.h"

static void putc_into(char *buf, u32 size, u32 *idx, char c) {
    if (*idx + 1 < size) buf[(*idx)++] = c;
}

static void puts_into(char *buf, u32 size, u32 *idx, const char *s) {
    if (!s) s = "(null)";
    while (*s) putc_into(buf, size, idx, *s++);
}

static void putu_into(char *buf, u32 size, u32 *idx, u32 v, int base, int upper) {
    char tmp[16];
    const char *dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int i = 0;
    if (v == 0) { putc_into(buf, size, idx, '0'); return; }
    while (v) { tmp[i++] = dig[v % base]; v /= base; }
    while (i--) putc_into(buf, size, idx, tmp[i]);
}

int kvsnprintf(char *buf, u32 size, const char *fmt, va_list args) {
    u32 idx = 0;
    if (size == 0) return 0;
    while (*fmt) {
        if (*fmt != '%') { putc_into(buf, size, &idx, *fmt++); continue; }
        fmt++;
        switch (*fmt) {
        case 'c': putc_into(buf, size, &idx, (char)va_arg(args, int)); break;
        case 's': puts_into(buf, size, &idx, va_arg(args, const char*)); break;
        case 'd': case 'i': {
            i32 v = va_arg(args, i32);
            if (v < 0) { putc_into(buf, size, &idx, '-'); v = -v; }
            putu_into(buf, size, &idx, (u32)v, 10, 0);
            break;
        }
        case 'u': putu_into(buf, size, &idx, va_arg(args, u32), 10, 0); break;
        case 'x': putu_into(buf, size, &idx, va_arg(args, u32), 16, 0); break;
        case 'X': putu_into(buf, size, &idx, va_arg(args, u32), 16, 1); break;
        case 'p': puts_into(buf, size, &idx, "0x");
                  putu_into(buf, size, &idx, va_arg(args, u32), 16, 0); break;
        case '%': putc_into(buf, size, &idx, '%'); break;
        default:  putc_into(buf, size, &idx, '%');
                  putc_into(buf, size, &idx, *fmt); break;
        }
        fmt++;
    }
    buf[idx] = '\0';
    return (int)idx;
}

int ksprintf(char *buf, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int n = kvsnprintf(buf, 0x1000, fmt, args);
    va_end(args);
    return n;
}

void kprintf(const char *fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    kvsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    serial_write(buf, (u32)strlen(buf));
}
