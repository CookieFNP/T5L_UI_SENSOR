#ifndef __SYS_H__
#define __SYS_H__
#include "t5los8051.h"
 
typedef unsigned char   u8;
typedef unsigned short  u16;
typedef unsigned long   u32;
typedef signed char     s8;
typedef signed short    s16;
typedef signed long     s32;

#define SYS_OK          1
#define SYS_ERR         0

 
#define	WDT_ON()				MUX_SEL|=0x02		 
#define	WDT_OFF()				MUX_SEL&=0xFD		 
#define	WDT_RST()				MUX_SEL|=0x01		 

 
#define FOSC     				206438400UL
#define T1MS    				(65536-FOSC/12/1000)
 
void sys_init(void);
void sys_delay_ms(u16 ms);
void sys_read_vp(u16 addr,u8* buf,u16 len);
void sys_write_vp(u16 addr,u8* buf,u16 len);

#endif


