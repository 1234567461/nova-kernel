/* ============================================================================
 * NovaOS - NovaPkg, the self-made package manager
 *
 * No apt, no pip, no external anything.  Packages are plain FAT files on the
 * NovaOS data disk and the whole system is three text/ELF file sets:
 *
 *   REPO.LST      - the package catalog:  <name> <version> <size> <desc...>
 *   PK<NAME>.ELF  - the package payloads (repo copies, e.g. PKHELLO.ELF)
 *   PKG.DB        - install database, one "name version" line per package
 *   <NAME>.ELF    - an installed package (runnable with the `run` command)
 *
 * `pkg install` copies the payload from the repo into place and records it
 * in PKG.DB; `pkg remove` deletes the ELF and rewrites the database.  The
 * catalog itself is what `pkg avail` prints.  Everything runs in the kernel
 * against the FAT12 driver - no external tools involved.
 * ========================================================================== */
#include "common.h"
#include "fat.h"
#include "string.h"
#include "printf.h"
#include "pkg.h"

#define PKG_BUF     (48 * 1024)     /* payload copy buffer */
#define REPO_FILE   "REPO.LST"
#define DB_FILE     "PKG.DB"

static char copybuf[PKG_BUF];

/* pull "<name> <version> <size> <desc>" out of REPO.LST */
static int repo_lookup(const char *name, char *ver, u32 vern,
                       char *desc, u32 descn) {
    static char repo[4096];
    u32 len = 0;
    if (!fat_read(REPO_FILE, (u8 *)repo, sizeof(repo) - 1, &len)) return 0;
    repo[len] = '\0';

    char *p = repo;
    while (*p) {
        char *line = p;
        while (*p && *p != '\n') p++;
        if (*p) *p++ = '\0';

        /* tokenize the line in place */
        char *tok = line;
        while (*tok == ' ') tok++;
        char *n = tok;
        while (*tok && *tok != ' ') tok++;
        if (*tok) *tok++ = '\0';
        if (strcmp(n, name) != 0) continue;

        while (*tok == ' ') tok++;
        char *v = tok;
        while (*tok && *tok != ' ') tok++;
        if (*tok) *tok++ = '\0';

        while (*tok == ' ') tok++;
        char *d = tok;

        u32 i = 0;
        while (v[i] && i < vern - 1) { ver[i] = v[i]; i++; }
        ver[i] = '\0';
        i = 0;
        while (d[i] && d[i] != '\r' && i < descn - 1) { desc[i] = d[i]; i++; }
        desc[i] = '\0';
        return 1;
    }
    return 0;
}

/* repo-side payload file name for a package ("hello" -> "PKHELLO.ELF") */
static void pkg_repo_name(const char *name, char *out, u32 n) {
    u32 i = 0;
    out[i++] = 'P';
    out[i++] = 'K';
    while (name[i - 2] && i < n - 5) { out[i] = name[i - 2]; i++; }
    /* append .ELF */
    out[i] = '\0';
    strcat(out, ".ELF");
    (void)n;
}

int pkg_installed(const char *name) {
    /* installed == <NAME>.ELF exists in the FAT root */
    char fn[FAT_NAME_LEN];
    u32 i = 0;
    while (name[i] && i < FAT_NAME_LEN - 5) { fn[i] = name[i]; i++; }
    fn[i] = '\0';
    strcat(fn, ".ELF");
    u32 size; u16 cl; u8 attr;
    return fat_stat(fn, &size, &cl, &attr);
}

int pkg_list_installed(void (*emit)(const char *name, const char *ver)) {
    static char db[2048];
    u32 len = 0;
    if (!fat_read(DB_FILE, (u8 *)db, sizeof(db) - 1, &len)) return 0;
    db[len] = '\0';
    char *p = db;
    while (*p) {
        char *line = p;
        while (*p && *p != '\n') p++;
        if (*p) *p++ = '\0';
        char *name = line;
        while (*line && *line != ' ') line++;
        if (*line) *line++ = '\0';
        char *ver = line;
        emit(name, ver);
    }
    return 1;
}

int pkg_avail(void (*emit)(const char *line)) {
    static char repo[4096];
    u32 len = 0;
    if (!fat_read(REPO_FILE, (u8 *)repo, sizeof(repo) - 1, &len)) return 0;
    repo[len] = '\0';
    char *p = repo;
    while (*p) {
        char *line = p;
        while (*p && *p != '\n') p++;
        if (*p) *p++ = '\0';
        char *tail = line + strlen(line);
        (void)tail;
        emit(line);
    }
    return 1;
}

int pkg_info(const char *name, char *out, u32 n) {
    char ver[16], desc[64];
    char rname[16];
    if (!repo_lookup(name, ver, sizeof(ver), desc, sizeof(desc))) return 0;
    pkg_repo_name(name, rname, sizeof(rname));
    u32 size; u16 cl; u8 attr;
    if (!fat_stat(rname, &size, &cl, &attr)) return 0;
    ksprintf(out, "%s %s, %u bytes - %s\n", name, ver, size, desc);
    (void)n;
    return 1;
}

int pkg_install(const char *name) {
    char ver[16], desc[64];
    char rname[FAT_NAME_LEN], fname[FAT_NAME_LEN];

    if (!repo_lookup(name, ver, sizeof(ver), desc, sizeof(desc))) return -1;
    pkg_repo_name(name, rname, sizeof(rname));

    u32 i = 0;
    while (name[i] && i < FAT_NAME_LEN - 5) { fname[i] = name[i]; i++; }
    fname[i] = '\0';
    strcat(fname, ".ELF");

    u32 size; u16 cl; u8 attr;
    if (!fat_stat(rname, &size, &cl, &attr)) return -2;   /* not in repo */
    if (size > PKG_BUF) return -3;

    u32 got = 0;
    if (!fat_read(rname, (u8 *)copybuf, PKG_BUF, &got)) return -4;
    if (!fat_write(fname, (u8 *)copybuf, got)) return -5;

    /* record in the install database (read + append + rewrite) */
    static char db[2048];
    u32 len = 0;
    fat_read(DB_FILE, (u8 *)db, sizeof(db) - 1, &len);
    db[len] = '\0';
    u32 w = len;
    if (w + strlen(name) + strlen(ver) + 3 >= sizeof(db)) return -6;
    for (u32 k = 0; name[k]; k++) db[w++] = name[k];
    db[w++] = ' ';
    for (u32 k = 0; ver[k]; k++) db[w++] = ver[k];
    db[w++] = '\n';
    if (!fat_write(DB_FILE, (u8 *)db, w)) return -7;
    return 0;
}

int pkg_remove(const char *name) {
    char fname[FAT_NAME_LEN];
    u32 i = 0;
    while (name[i] && i < FAT_NAME_LEN - 5) { fname[i] = name[i]; i++; }
    fname[i] = '\0';
    strcat(fname, ".ELF");

    if (!fat_delete(fname)) return -1;

    /* rewrite PKG.DB without the entry */
    static char db[2048], out[2048];
    u32 len = 0, w = 0;
    if (fat_read(DB_FILE, (u8 *)db, sizeof(db) - 1, &len)) {
        db[len] = '\0';
        char *p = db;
        while (*p) {
            char *line = p;
            while (*p && *p != '\n') p++;
            if (*p) *p++ = '\0';
            if (strncmp(line, name, strlen(name)) == 0 &&
                (line[strlen(name)] == ' ' || line[strlen(name)] == '\0'))
                continue;
            for (u32 k = 0; line[k]; k++) out[w++] = line[k];
            out[w++] = '\n';
        }
    }
    fat_write(DB_FILE, (u8 *)out, w);
    return 0;
}
