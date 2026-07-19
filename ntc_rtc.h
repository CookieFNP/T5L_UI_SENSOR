#ifndef __NTC_RTC_H__
#define __NTC_RTC_H__

#include "sys.h"

typedef struct _rtc_time
{
    u8 year;
    u8 month;
    u8 day;
    u8 week;
    u8 hour;
    u8 min;
    u8 sec;
    u8 fault;
} rtc_time_t;

void ntc_init(void);
s16 ntc_read_external_x10(void);

void rtc_init(void);
u8 rtc_read(rtc_time_t *out);
u8 rtc_set_time(rtc_time_t *time);

#endif
