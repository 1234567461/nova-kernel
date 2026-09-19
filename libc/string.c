/* NovaOS libc - string and memory primitives */
#include "string.h"

void *memcpy(void *dst, const void *src, u32 n) {
    u8 *d = (u8*)dst;
    const u8 *s = (const u8*)src;
    while (n--) *d++ = *s++;
    return dst;
}

void *memset(void *dst, int c, u32 n) {
    u8 *d = (u8*)dst;
    while (n--) *d++ = (u8)c;
    return dst;
}

int memcmp(const void *a, const void *b, u32 n) {
    const u8 *x = (const u8*)a;
    const u8 *y = (const u8*)b;
    while (n--) {
        if (*x != *y) return (int)*x - (int)*y;
        x++; y++;
    }
    return 0;
}

u32 strlen(const char *s) {
    u32 n = 0;
    while (*s++) n++;
    return n;
}

int strcmp(const char *a, const char *b) {
    while (*a && (*a == *b)) { a++; b++; }
    return (int)(u8)*a - (int)(u8)*b;
}

int strncmp(const char *a, const char *b, u32 n) {
    while (n-- && *a && (*a == *b)) { a++; b++; }
    if (n == (u32)-1) return 0;
    return (int)(u8)*a - (int)(u8)*b;
}

char *strcpy(char *dst, const char *src) {
    char *d = dst;
    while ((*d++ = *src++)) ;
    return dst;
}

char *strncpy(char *dst, const char *src, u32 n) {
    char *d = dst;
    while (n && (*d++ = *src++)) n--;
    while (n--) *d++ = '\0';
    return dst;
}

char *strcat(char *dst, const char *src) {
    char *d = dst;
    while (*d) d++;
    while ((*d++ = *src++)) ;
    return dst;
}

char *strchr(const char *s, int c) {
    while (*s) {
        if (*s == (char)c) return (char*)s;
        s++;
    }
    return (c == 0) ? (char*)s : NULL;
}
