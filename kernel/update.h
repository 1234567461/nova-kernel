/* NovaOS - over-the-air system updater */
#ifndef NOVA_UPDATE_H
#define NOVA_UPDATE_H

#include "common.h"

#define UPD_PORT 9999

/* result codes for upd_check() */
#define UPD_OK        0
#define UPD_NO_LINK  -1
#define UPD_TIMEOUT  -2
#define UPD_BAD_CRC  -3
#define UPD_WRITE    -4
#define UPD_PARSE    -5

void        upd_init(void);
int         upd_check(void);            /* fetch + verify + install */
const char *upd_version(void);
const char *upd_channel(void);
const char *upd_source(void);           /* "factory" | "10.0.2.2" | "serial" */
const char *upd_notes(void);
u32         upd_size(void);
u32         upd_crc(void);
u32         upd_checks(void);
int         upd_available(void);        /* bundle arrived but not installed? */

#endif
