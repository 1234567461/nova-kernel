/* NovaOS libc - string and memory primitives (freestanding) */
#ifndef NOVA_STRING_H
#define NOVA_STRING_H

#include "common.h"

void  *memcpy(void *dst, const void *src, u32 n);
void  *memset(void *dst, int c, u32 n);
int    memcmp(const void *a, const void *b, u32 n);
u32    strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, u32 n);
char  *strcpy(char *dst, const char *src);
char  *strncpy(char *dst, const char *src, u32 n);
char  *strcat(char *dst, const char *src);
char  *strchr(const char *s, int c);

#endif
