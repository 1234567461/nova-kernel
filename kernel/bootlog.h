/* NovaOS - boot log
 *
 * Pretty, timestamped boot sequence output: "[ 0.012s] paging: COW .... ok"
 * with the step name in white, the filler dots dim and the result in green
 * (or red on failure).  Everything is mirrored to the serial console in
 * plain text so headless runs are still readable.
 */
#ifndef NOVA_BOOTLOG_H
#define NOVA_BOOTLOG_H

void bootlog_banner(void);           /* the version banner */
void bootlog_start(const char *step, const char *detail);
                                     /* "[ 0.000s] paging: per-process ...." */
void bootlog_ok(const char *note);   /* appends " ok (note)" in green */
void bootlog_fail(const char *note); /* appends " FAIL (note)" in red */
void bootlog_done(void);             /* "boot complete in 0.0xx s" */

#endif
