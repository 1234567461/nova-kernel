/* NovaOS - minimal IPv4 stack (ARP + ICMP echo + UDP) over NE2000 */
#ifndef NOVA_NET_H
#define NOVA_NET_H

#include "common.h"

#define NET_IP(a,b,c,d)  (((u32)(a) << 24) | ((u32)(b) << 16) | ((u32)(c) << 8) | (u32)(d))

typedef void (*net_udp_cb_t)(u32 src_ip, u16 sport, u16 dport,
                             const u8 *data, u16 len);

int   net_init(void);
int   net_up(void);
void  net_poll(void);                       /* drain rx ring, dispatch */
int   net_udp_send(u32 dst_ip, u16 sport, u16 dport, const u8 *data, u16 len);
void  net_set_udp_handler(net_udp_cb_t cb);
int   net_ping(u32 dst_ip, u32 timeout_ms); /* 1 = reply, 0 = timeout */
void  net_get_ip(u32 *ip, u32 *gw, u32 *mask);
void  net_set_ip(u32 ip, u32 gw, u32 mask);
void  net_get_mac(u8 out[6]);
u32   net_rx_frames(void);
u32   net_tx_frames(void);
const char *net_driver_name(void);
void  net_debug(void);
u16   net_io_base(void);
int   net_irq(void);

#endif
