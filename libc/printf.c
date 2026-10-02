/* NovaOS libc - minimal formatting engine.
 *
 * Supports: %c %s %d %i %u %x %X %p %% plus the standard flags/width/
 * precision subset a console actually needs:
 *     %-14s   left justify, pad to 14
 *     %08x    zero pad to 8
 *     %2u     space pad to 2
 *     %5s     right justify
 *     %.3d    (precision on integers means "at least N digits")
 * No floats - kept deliberately small.
 *
 * The width/precision parsing lives here because without it every padded
 * format string ("%-14s %8u") printed *literally*, which made the shell's
 * ls / ps / hexdump output look like raw format text instead of a table.
 */
#include "printf.h"
#include "string.h"
#include "../kernel/serial.h"

#define PAD_CHUNK 64

static void putc_into(char *buf, u32 size, u32 *idx, char c) {
    if (size == 0) return;
    if (*idx + 1 < size) buf[*idx] = c;
    (*idx)++;
}

/* emit `count` copies of `c` */
static void putpad(char *buf, u32 size, u32 *idx, char c, int count) {
    for (int i = 0; i < count; i++) putc_into(buf, size, idx, c);
}

static int u32_to_str(u32 v, int base, int upper, char *out) {
    const char *dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[32];
    int i = 0;
    if (v == 0) tmp[i++] = '0';
    while (v) { tmp[i++] = dig[v % (u32)base]; v /= (u32)base; }
    for (int k = 0; k < i; k++) out[k] = tmp[i - 1 - k];
    out[i] = '\0';
    return i;
}

int kvsnprintf(char *buf, u32 size, const char *fmt, va_list args) {
    u32 idx = 0;
    if (size == 0) return 0;

    while (*fmt) {
        if (*fmt != '%') { putc_into(buf, size, &idx, *fmt++); continue; }
        fmt++;

        /* ---- flags ---- */
        int left = 0, zero = 0;
        for (;;) {
            if (*fmt == '-') { left = 1; fmt++; }
            else if (*fmt == '0') { zero = 1; fmt++; }
            else break;
        }

        /* ---- width ---- */
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt - '0'); fmt++; }

        /* ---- precision ---- */
        int prec = -1;
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            while (*fmt >= '0' && *fmt <= '9') { prec = prec * 10 + (*fmt - '0'); fmt++; }
        }

        /* ---- conversion ---- */
        char tmp[80];
        int len = 0;
        int negative = 0;
        char pad = zero && !left ? '0' : ' ';

        switch (*fmt) {
        case 'c': {
            tmp[len++] = (char)va_arg(args, int);
            break;
        }
        case 's': {
            const char *s = va_arg(args, const char*);
            if (!s) s = "(null)";
            while (s[len] && (prec < 0 || len < prec)) len++;
            for (int i = 0; i < len; i++) tmp[i] = s[i];
            break;
        }
        case 'd': case 'i': {
            i32 v = va_arg(args, i32);
            if (v < 0) { negative = 1; v = -v; }
            char num[32];
            len = u32_to_str((u32)v, 10, 0, num);
            /* precision = minimum digits, zero filled */
            if (prec > len) {
                int z = prec - len;
                for (int i = 0; i < z; i++) tmp[i] = '0';
                for (int i = 0; i < len; i++) tmp[z + i] = num[i];
                len += z;
            } else {
                for (int i = 0; i < len; i++) tmp[i] = num[i];
            }
            break;
        }
        case 'u': case 'x': case 'X': {
            u32 v = va_arg(args, u32);
            int base = (*fmt == 'u') ? 10 : 16;
            int up   = (*fmt == 'X');
            char num[32];
            len = u32_to_str(v, base, up, num);
            if (prec > len) {
                int z = prec - len;
                for (int i = 0; i < z; i++) tmp[i] = '0';
                for (int i = 0; i < len; i++) tmp[z + i] = num[i];
                len += z;
            } else {
                for (int i = 0; i < len; i++) tmp[i] = num[i];
            }
            break;
        }
        case 'p': {
            u32 v = va_arg(args, u32);
            tmp[len++] = '0';
            tmp[len++] = 'x';
            char num[32];
            int n = u32_to_str(v, 16, 0, num);
            for (int i = 0; i < n; i++) tmp[len++] = num[i];
            break;
        }
        case '%': {
            tmp[len++] = '%';
            break;
        }
        case '\0':  /* trailing '%' - print it and stop */
            putc_into(buf, size, &idx, '%');
            buf[idx < size ? idx : size - 1] = '\0';
            return (int)idx;
        default:
            tmp[len++] = '%';
            tmp[len++] = *fmt;
            break;
        }

        /* total tokens to place = sign + digits */
        int body = len + (negative ? 1 : 0);
        int padding = (width > body) ? width - body : 0;

        if (!left && pad == ' ') putpad(buf, size, &idx, ' ', padding);
        if (negative)            putc_into(buf, size, &idx, '-');
        if (!left && pad == '0' && prec < 0) putpad(buf, size, &idx, '0', padding);
        for (int i = 0; i < len; i++) putc_into(buf, size, &idx, tmp[i]);
        if (left) putpad(buf, size, &idx, ' ', padding);
        (void)PAD_CHUNK;

        fmt++;
    }
    buf[idx < size ? idx : size - 1] = '\0';
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
