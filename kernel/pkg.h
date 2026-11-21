#ifndef NOVA_PKG_H
#define NOVA_PKG_H
#include "common.h"

/* NovaPkg - self-contained package manager (see pkg.c for the format) */

int pkg_installed(const char *name);
int pkg_avail(void (*emit)(const char *line));
int pkg_info(const char *name, char *out, u32 n);
int pkg_install(const char *name);   /* 0 ok, <0 error code */
int pkg_remove(const char *name);    /* 0 ok, <0 error code */
int pkg_list_installed(void (*emit)(const char *name, const char *ver));

#endif
