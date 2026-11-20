/* NovaOS - minimal IPv4 stack: ARP, ICMP echo, UDP
 *
 * Deliberately small: no fragmentation, no TCP, no DHCP.  Enough for the
 * updater to talk to a plain UDP server and for `ping` to prove the link
 * really carries packets.
 */
#include "net.h"
#include "ne2k.h"
#include "rtl8139.h"
#include "string.h"
#include "printf.h"
#include "io.h"

extern u32 timer_ticks(void);

#define ETH_ARP 0x0806
#define ETH_IP  0x0800
#define PROTO_ICMP 1
#define PROTO_UDP  17

struct eth_hdr {
    u8  dst[6];
    u8  src[6];
    u16 type;
} __attribute__((packed));

struct arp_pkt {
    u16 htype, ptype;
    u8  hlen, plen;
    u16 op;                 /* 1 request, 2 reply */
    u8  sha[6];
    u32 spa;
    u8  tha[6];
    u32 tpa;
} __attribute__((packed));

struct ip_hdr {
    u8  vihl, tos;
    u16 tot_len, id, frag;
    u8  ttl, proto;
    u16 csum;
    u32 src, dst;
} __attribute__((packed));

struct icmp_hdr {
    u8  type, code;
    u16 csum, id, seq;
} __attribute__((packed));

struct udp_hdr {
    u16 sport, dport, len, csum;
} __attribute__((packed));

static u32 my_ip = NET_IP(10, 0, 2, 15);
static u32 gw_ip = NET_IP(10, 0, 2, 2);
static u32 net_mask = NET_IP(255, 255, 255, 0);
static u8  my_mac[6];
static u8  gw_mac[6];
static int have_gw_mac;
static int up;

static u16 ip_id = 0x1234;
static u16 icmp_seq = 1;
static volatile int ping_reply;
static volatile u32 ping_from;

static u8 txbuf[1600];
static u8 rxbuf[1600];
static net_udp_cb_t udp_cb;
static u32 rx_frames, tx_frames;

/* the bound driver */
static int  (*drv_send)(const u8 *frame, u16 len);
static int  (*drv_recv)(u8 *buf, u16 max);
static const char *drv_name = "none";
static u16 drv_io;
static int drv_irq;

static const u8 bcast_mac[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

static inline u16 htons(u16 v) { return (u16)((v >> 8) | (v << 8)); }
static inline u32 htonl(u32 v) {
    return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
           ((v & 0x00FF0000u) >> 8)  | ((v & 0xFF000000u) >> 24);
}
#define ntohl(v) htonl(v)
#define ntohs(v) htons(v)

static u16 csum16(const u8 *p, u32 len) {
    u32 sum = 0;
    while (len > 1) { sum += (u16)(p[0] | ((u16)p[1] << 8)); p += 2; len -= 2; }
    if (len) sum += p[0];
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (u16)(~sum & 0xFFFF);
}

static void eth_send(const u8 *dst, u16 type, const u8 *payload, u16 len) {
    if (!up) return;
    if (len > 1500) len = 1500;

    struct eth_hdr *e = (struct eth_hdr*)txbuf;
    memcpy(e->dst, dst, 6);
    memcpy(e->src, my_mac, 6);
    e->type = htons(type);
    memcpy(txbuf + sizeof(struct eth_hdr), payload, len);

    u16 total = (u16)(sizeof(struct eth_hdr) + len);
    if (total < 60) {                       /* pad to the ethernet minimum */
        memset(txbuf + total, 0, 60 - total);
        total = 60;
    }
    if (drv_send && drv_send(txbuf, total)) tx_frames++;
}

/* --- ARP -------------------------------------------------------------- */
static void arp_send(u16 op, const u8 *dst_mac, u32 target_ip) {
    struct arp_pkt a;
    a.htype = htons(1);
    a.ptype = htons(ETH_IP);
    a.hlen = 6; a.plen = 4;
    a.op = htons(op);
    memcpy(a.sha, my_mac, 6);
    a.spa = htonl(my_ip);                   /* struct fields are wire order */
    memcpy(a.tha, dst_mac, 6);
    a.tpa = htonl(target_ip);
    eth_send(dst_mac, ETH_ARP, (const u8*)&a, sizeof(a));
}

static int arp_resolve(u32 ip, u8 out[6], u32 timeout_ms) {
    if (ip == my_ip) { memcpy(out, my_mac, 6); return 1; }
    if (have_gw_mac && ip == gw_ip) { memcpy(out, gw_mac, 6); return 1; }

    u32 start = timer_ticks();
    u32 ticks = timeout_ms / 10;
    while ((timer_ticks() - start) < ticks) {
        arp_send(1, bcast_mac, ip);
        for (int i = 0; i < 20; i++) {
            net_poll();
            if (have_gw_mac && ip == gw_ip) { memcpy(out, gw_mac, 6); return 1; }
            for (volatile int k = 0; k < 2000; k++) io_wait();
            if ((timer_ticks() - start) >= ticks) break;
        }
    }
    return 0;
}

/* --- IPv4 ------------------------------------------------------------- */
static int ip_send(u32 dst_ip, u8 proto, const u8 *payload, u16 len) {
    u8 dst_mac[6];

    /* off-subnet traffic goes to the gateway */
    u32 hop = ((dst_ip & net_mask) == (my_ip & net_mask)) ? dst_ip : gw_ip;
    if (!arp_resolve(hop, dst_mac, 500)) {
        /* Some virtual switches (QEMU user-net in particular) answer the
         * ARP request only after they have seen an IP frame from us.  Fall
         * back to the link broadcast address: a gateway always accepts a
         * frame addressed to it at layer 2 when the IP destination is its
         * own address, so this costs one broadcast and keeps the flow
         * alive.  Real hardware also handles it (the switch floods). */
        memcpy(dst_mac, bcast_mac, 6);
    }

    static u8 pkt[1600];
    struct ip_hdr *ip = (struct ip_hdr*)pkt;
    ip->vihl = 0x45;
    ip->tos = 0;
    ip->tot_len = htons((u16)(sizeof(struct ip_hdr) + len));
    ip->id = htons(ip_id++);
    ip->frag = htons(0x4000);               /* don't fragment */
    ip->ttl = 64;
    ip->proto = proto;
    ip->csum = 0;
    ip->src = htonl(my_ip);
    ip->dst = htonl(dst_ip);
    ip->csum = csum16((const u8*)ip, sizeof(struct ip_hdr));
    memcpy(pkt + sizeof(struct ip_hdr), payload, len);

    eth_send(dst_mac, ETH_IP, pkt, (u16)(sizeof(struct ip_hdr) + len));
    return 1;
}

int net_udp_send(u32 dst_ip, u16 sport, u16 dport, const u8 *data, u16 len) {
    static u8 pkt[1500];
    if (len > 1472) return 0;
    struct udp_hdr *u = (struct udp_hdr*)pkt;
    u->sport = htons(sport);
    u->dport = htons(dport);
    u->len   = htons((u16)(sizeof(struct udp_hdr) + len));
    u->csum  = 0;                           /* optional in IPv4 */
    memcpy(pkt + sizeof(struct udp_hdr), data, len);
    return ip_send(dst_ip, PROTO_UDP, pkt, (u16)(sizeof(struct udp_hdr) + len));
}

int net_ping(u32 dst_ip, u32 timeout_ms) {
    static u8 pkt[64];
    struct icmp_hdr *ic = (struct icmp_hdr*)pkt;
    u16 seq = icmp_seq++;

    ic->type = 8;                           /* echo request */
    ic->code = 0;
    ic->csum = 0;
    ic->id = htons(0x4E56);                 /* 'NV' */
    ic->seq = htons(seq);
    for (int i = sizeof(struct icmp_hdr); i < 32; i++) pkt[i] = (u8)i;
    ic->csum = csum16(pkt, 32);

    ping_reply = 0;
    if (!ip_send(dst_ip, PROTO_ICMP, pkt, 32)) return 0;

    u32 start = timer_ticks();
    while ((timer_ticks() - start) < timeout_ms / 10) {
        net_poll();
        if (ping_reply) return 1;
        for (volatile int k = 0; k < 5000; k++) io_wait();
    }
    return 0;
}

/* --- receive path ----------------------------------------------------- */
static void handle_arp(const u8 *p, u16 len) {
    if (len < (int)sizeof(struct arp_pkt)) return;
    const struct arp_pkt *a = (const struct arp_pkt*)p;
    u16 op = htons(a->op);

    if (htons(a->ptype) != ETH_IP) return;

    if (op == 1 && ntohl(a->tpa) == my_ip) {   /* somebody asks for us */
        static u8 reply[64];
        struct arp_pkt *r = (struct arp_pkt*)reply;
        r->htype = htons(1);
        r->ptype = htons(ETH_IP);
        r->hlen = 6; r->plen = 4;
        r->op = htons(2);
        memcpy(r->sha, my_mac, 6);
        r->spa = htonl(my_ip);
        memcpy(r->tha, a->sha, 6);
        r->tpa = a->spa;                    /* already wire order */
        eth_send(a->sha, ETH_ARP, reply, sizeof(struct arp_pkt));
    } else if (op == 2) {                   /* reply: learn the gateway */
        if (ntohl(a->spa) == gw_ip) {
            memcpy(gw_mac, a->sha, 6);
            have_gw_mac = 1;
        }
    }
}

static void handle_icmp(u32 src, const u8 *p, u16 len) {
    if (len < (int)sizeof(struct icmp_hdr)) return;
    const struct icmp_hdr *ic = (const struct icmp_hdr*)p;
    if (ic->type == 0 && htons(ic->id) == 0x4E56) {  /* echo reply */
        ping_reply = 1;
        ping_from = src;
    } else if (ic->type == 8) {             /* echo request to us: answer */
        static u8 pkt[1600];
        u16 n = len > 1472 ? 1472 : len;
        memcpy(pkt, p, n);
        struct icmp_hdr *r = (struct icmp_hdr*)pkt;
        r->type = 0;
        r->code = 0;
        r->csum = 0;
        r->csum = csum16(pkt, n);
        ip_send(src, PROTO_ICMP, pkt, n);
    }
}

static void handle_ip(const u8 *p, u16 len) {
    if (len < (int)sizeof(struct ip_hdr)) return;
    const struct ip_hdr *ip = (const struct ip_hdr*)p;
    if ((ip->vihl >> 4) != 4) return;

    u16 ihl = (u16)((ip->vihl & 0x0F) * 4);
    u16 tot = htons(ip->tot_len);
    if (ihl < 20 || tot > len) return;

    u32 dst = ntohl(ip->dst);
    if (dst != my_ip && dst != 0xFFFFFFFFu) return;

    const u8 *payload = p + ihl;
    u16 plen = (u16)(tot - ihl);

    u32 src = ntohl(ip->src);

    if (ip->proto == PROTO_ICMP) {
        handle_icmp(src, payload, plen);
    } else if (ip->proto == PROTO_UDP) {
        if (plen < (int)sizeof(struct udp_hdr)) return;
        const struct udp_hdr *u = (const struct udp_hdr*)payload;
        u16 ulen = htons(u->len);
        if (ulen < sizeof(struct udp_hdr) || ulen > plen) return;
        if (udp_cb)
            udp_cb(src, htons(u->sport), htons(u->dport),
                   payload + sizeof(struct udp_hdr),
                   (u16)(ulen - sizeof(struct udp_hdr)));
    }
}

void net_poll(void) {
    if (!up) return;
    for (int guard = 0; guard < 16; guard++) {
        int n = drv_recv ? drv_recv(rxbuf, sizeof(rxbuf)) : 0;
        if (n <= 0) break;
        rx_frames++;

        if (n < (int)sizeof(struct eth_hdr)) continue;
        const struct eth_hdr *e = (const struct eth_hdr*)rxbuf;
        u16 type = htons(e->type);
        const u8 *payload = rxbuf + sizeof(struct eth_hdr);
        u16 plen = (u16)(n - sizeof(struct eth_hdr));

        if (type == ETH_ARP)      handle_arp(payload, plen);
        else if (type == ETH_IP)  handle_ip(payload, plen);
    }
}

int net_init(void) {
    up = 0;
    have_gw_mac = 0;

    if (rtl_init()) {
        drv_send = rtl_send; drv_recv = rtl_recv; rtl_mac(my_mac);
        drv_name = "rtl8139"; drv_io = rtl_io_base(); drv_irq = rtl_irq_line();
    } else if (ne2k_init()) {
        drv_send = ne2k_send; drv_recv = ne2k_recv; ne2k_mac(my_mac);
        drv_name = "ne2000"; drv_io = ne2k_io_base(); drv_irq = ne2k_irq_line();
    } else {
        return 0;
    }
    up = 1;
    /* No ARP here: net_init() runs before sti(), so timer_ticks() would
     * never advance and arp_resolve() would spin forever.  The first
     * real use (net_probe / update) resolves the gateway with interrupts
     * live. */
    return 1;
}

int net_up(void) { return up; }
void net_set_udp_handler(net_udp_cb_t cb) { udp_cb = cb; }
void net_get_ip(u32 *ip, u32 *gw, u32 *mask) {
    if (ip)   *ip   = my_ip;
    if (gw)   *gw   = gw_ip;
    if (mask) *mask = net_mask;
}
void net_set_ip(u32 ip, u32 gw, u32 mask) { my_ip = ip; gw_ip = gw; net_mask = mask; have_gw_mac = 0; }
void net_get_mac(u8 out[6]) { memcpy(out, my_mac, 6); }
u32 net_rx_frames(void) { return rx_frames; }
u32 net_tx_frames(void) { return tx_frames; }
const char *net_driver_name(void) { return drv_name; }
void net_debug(void) { if (drv_name[0]=='r') rtl_dbg(); }
u16 net_io_base(void) { return drv_io; }
int net_irq(void)     { return drv_irq; }
