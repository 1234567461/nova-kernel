/* NovaOS - RTL8139 ethernet driver (polled) */
#ifndef NOVA_RTL8139_H
#define NOVA_RTL8139_H

#include "common.h"

int  rtl_init(void);
void rtl_dbg(void);
int  rtl_present(void);
int  rtl_send(const u8 *frame, u16 len);
int  rtl_recv(u8 *buf, u16 max);
void rtl_mac(u8 out[6]);
u16  rtl_io_base(void);
int  rtl_irq_line(void);
u32  rtl_tx_count(void);
u32  rtl_rx_count(void);

#endif
