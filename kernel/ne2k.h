/* NovaOS - NE2000 (DP8390) ethernet driver */
#ifndef NOVA_NE2K_H
#define NOVA_NE2K_H

#include "common.h"

int  ne2k_init(void);
int  ne2k_present(void);
int  ne2k_send(const u8 *frame, u16 len);          /* returns 0 on timeout */
int  ne2k_recv(u8 *buf, u16 max);                  /* frame length or 0 */
void ne2k_mac(u8 out[6]);
u16  ne2k_io_base(void);
u32  ne2k_tx_count(void);
u32  ne2k_rx_count(void);
int  ne2k_irq_line(void);

#endif
