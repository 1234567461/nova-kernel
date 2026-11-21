/* NovaOS - over-the-air system updater
 *
 * Two transports, one protocol:
 *
 *   UDP   10.0.2.2:9999 (QEMU slirp / anything routable).  Request is the
 *         ASCII line "NVUP1 GET <version>\n"; the server answers with
 *          META / DATA / END datagrams.
 *   SERIAL  the same META/DATA/END exchange, framed by ESC "NVUP1", for
 *         machines without a NIC - notably the browser build, where the
 *         page itself plays the update server.
 *
 * A bundle is plain text:
 *
 *   NOVA-UPD 1
 *   version=0.8.0
 *   channel=stable
 *   feature=...
 *   note=...
 *
 * Once the CRC32 checks out the bundle is written to UPDATE.BIN on the FAT
 * volume and parsed in place, so version/channel/notes change immediately -
 * that is the "hot" part of the update: no reboot, no reflash.
 */
#include "update.h"
#include "net.h"
#include "serial.h"
#include "fat.h"
#include "string.h"
#include "printf.h"
#include "io.h"

extern u32 timer_ticks(void);

#define UPD_MAX 4096

static u8  bundle[UPD_MAX + 64];
static u32 bundle_len;

static char cur_version[16] = "0.8";
static char cur_channel[16] = "building";
static char cur_source[24] = "factory";
static char notes[320];
static u32  last_crc, last_size, checks;

static u32 want_crc, want_total;
static int want_chunks, got_chunks;
static u8  seen[32];
static volatile int meta_ok, end_ok;

/* --- helpers ---------------------------------------------------------- */
static u32 crc32(const u8 *p, u32 n) {
    u32 c = 0xFFFFFFFFu;
    for (u32 i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & (u32)(-(int)(c & 1u)));
    }
    return c ^ 0xFFFFFFFFu;
}

static u32 parse_num(const char **pp) {
    const char *p = *pp;
    u32 v = 0;
    while (*p == ' ') p++;
    while (*p >= '0' && *p <= '9') { v = v * 10 + (u32)(*p - '0'); p++; }
    *pp = p;
    return v;
}

static u32 parse_hex(const char **pp) {
    const char *p = *pp;
    u32 v = 0;
    while (*p == ' ') p++;
    for (;;) {
        char c = *p;
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else break;
        v = (v << 4) | (u32)d;
        p++;
    }
    *pp = p;
    return v;
}

static void copy_field(char *dst, u32 cap, const char *src) {
    u32 i = 0;
    while (src[i] && src[i] != '\n' && src[i] != '\r' && i + 1 < cap) {
        dst[i] = src[i]; i++;
    }
    dst[i] = '\0';
}

/* --- bundle parsing --------------------------------------------------- */
static void parse_bundle(void) {
    const char *p = (const char*)bundle;
    const char *end = (const char*)bundle + bundle_len;

    notes[0] = '\0';

    while (p < end) {
        const char *nl = strchr(p, '\n');
        if (!nl || nl > end) nl = end;

        if (!strncmp(p, "version=", 8))      copy_field(cur_version, sizeof(cur_version), p + 8);
        else if (!strncmp(p, "channel=", 8)) copy_field(cur_channel, sizeof(cur_channel), p + 8);
        else if (!strncmp(p, "note=", 5)) {
            u32 have = strlen(notes);
            if (have + 2 < sizeof(notes)) {
                if (have) { notes[have++] = '\n'; notes[have] = '\0'; }
                u32 room = (u32)sizeof(notes) - have - 1;
                u32 n = (u32)(nl - (p + 5));
                if (n > room) n = room;
                memcpy(notes + have, p + 5, n);
                notes[have + n] = '\0';
            }
        }
        p = (nl < end) ? nl + 1 : end;
    }
}

/* --- UDP transport ---------------------------------------------------- */
static void on_udp(u32 src, u16 sport, u16 dport, const u8 *d, u16 len) {
    (void)src; (void)dport;
    /* replies come FROM the server port; the destination is whatever
     * ephemeral port we picked, so filtering on dport drops every packet */
    if (sport != UPD_PORT) return;
    if (len < 8 || memcmp(d, "NVUP1 ", 6) != 0) return;

    const char *s = (const char*)d + 6;

    if (!strncmp(s, "META", 4)) {
        const char *p = s + 4;
        want_total  = parse_num(&p);
        want_crc    = parse_hex(&p);
        want_chunks = (int)parse_num(&p);
        if (want_chunks < 0 || want_chunks > 32) want_chunks = 0;
        for (int i = 0; i < 32; i++) seen[i] = 0;
        got_chunks = 0;
        bundle_len = 0;
        meta_ok = 1;
    } else if (!strncmp(s, "DATA", 4)) {
        const char *p = s + 4;
        u32 seq = parse_num(&p);
        const u8 *nl = (const u8*)strchr((const char*)p, '\n');
        if (!nl || nl + 1 > d + len) return;
        const u8 *payload = nl + 1;
        u16 plen = (u16)((d + len) - payload);

        u32 off = seq * 1024;
        if (off + plen > UPD_MAX) return;
        memcpy(bundle + off, payload, plen);
        if (off + plen > bundle_len) bundle_len = off + plen;

        if (seq < 32 && !seen[seq]) { seen[seq] = 1; got_chunks++; }
    } else if (!strncmp(s, "END", 3)) {
        end_ok = 1;
    }
}

/* --- serial transport (browser update server) ------------------------- */
static int serial_fetch(u32 timeout_ms) {
    static const char req[] = "\x1bNVUP1 GET\n";
    serial_puts(req);

    u32 start = timer_ticks();
    int  state = 0;                 /* 0 = hunting for the frame start */
    u32  n = 0;
    static const char tag[] = "\x1bNVUP1 ";

    while ((timer_ticks() - start) < timeout_ms / 10) {
        int c = serial_getc();
        if (c < 0) continue;

        if (state == 0) {
            if (c == 0x1B) { state = 1; n = 0; }
            continue;
        }
        /* collect until the header tag matched, then stream the bundle */
        if (n < 6) {
            if (c == tag[n + 1]) { n++; continue; }
            state = 0; n = 0;
            if (c == 0x1B) { state = 1; }
            continue;
        }
        /* after "\x1bNVUP1 " the server sends the bundle text */
        if (n < 6 + UPD_MAX) bundle[n - 6] = (u8)c;
        n++;
        if (c == 0x1B) {                 /* possible trailing END marker */
            bundle_len = n - 6 - 1;
            return 1;
        }
    }
    return 0;
}

/* --- public API ------------------------------------------------------- */
void upd_init(void) {
    net_set_udp_handler(on_udp);
    checks = 0;
}

int upd_check(void) {
    checks++;
    meta_ok = end_ok = 0;
    bundle_len = 0;
    want_total = want_crc = 0;
    want_chunks = got_chunks = 0;

    int use_net = net_up();
    if (use_net) {
        static char req[48];
        ksprintf(req, "NVUP1 GET %s\n", cur_version);
        if (!net_udp_send(NET_IP(10,0,2,2), 40000, UPD_PORT,
                          (const u8*)req, (u16)strlen(req)))
            use_net = 0;
    }

    if (use_net) {
        u32 start = timer_ticks();
        while ((timer_ticks() - start) < 400) {     /* 4 s */
            net_poll();
            if (end_ok && meta_ok && got_chunks >= want_chunks) break;
            for (volatile int k = 0; k < 3000; k++) io_wait();
        }
        if (!meta_ok || !end_ok) {
            /* fall back to the serial console */
            if (!serial_fetch(2500)) return UPD_TIMEOUT;
            copy_field(cur_source, sizeof(cur_source), "serial");
        } else {
            copy_field(cur_source, sizeof(cur_source), "10.0.2.2");
        }
    } else {
        if (!serial_fetch(2500)) return UPD_NO_LINK;
        copy_field(cur_source, sizeof(cur_source), "serial");
    }

    if (bundle_len < 16) return UPD_PARSE;
    if (bundle_len > UPD_MAX) bundle_len = UPD_MAX;

    u32 crc = crc32(bundle, bundle_len);
    if (want_crc && crc != want_crc) return UPD_BAD_CRC;
    if (want_total && bundle_len != want_total) return UPD_BAD_CRC;

    last_crc = crc;
    last_size = bundle_len;

    if (fat_mounted()) {
        if (!fat_write("UPDATE.BIN", bundle, bundle_len)) return UPD_WRITE;
    }

    parse_bundle();
    return UPD_OK;
}

const char *upd_version(void) { return cur_version; }
const char *upd_channel(void) { return cur_channel; }
const char *upd_source(void)  { return cur_source; }
const char *upd_notes(void)   { return notes; }
u32 upd_size(void)   { return last_size; }
u32 upd_crc(void)    { return last_crc; }
u32 upd_checks(void) { return checks; }
int upd_available(void) { return bundle_len > 0; }
