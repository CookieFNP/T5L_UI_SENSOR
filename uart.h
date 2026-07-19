#ifndef __UART_H__
#define __UART_H__

#include "sys.h"

void uart_init(void);
void uart4_init(void);
void uart5_init(void);
u8 uart4_send_byte(u8 dat);
u8 uart5_send_byte(u8 dat);
u8 uart4_send(u8 *buf, u16 len);
u8 uart5_send(u8 *buf, u16 len);
u32 uart_status(void);
u32 uart4_tx_bytes(void);
u32 uart5_tx_bytes(void);
u16 uart4_tx_frames(void);
u16 uart5_tx_frames(void);

#endif
