#include "ntc_rtc.h"

sbit RTC_SCL_PORT = P3^2;
sbit RTC_SDA_PORT = P3^3;

#define NTC_TABLE_SIZE        100
#define NTC_ADC_VP_ADDR       0x0032
#define NTC_PULL_RES_OHM      10000UL
#define NTC_ADC_FULL_SCALE    4095UL
#define NTC_SAMPLE_COUNT      64
#define NTC_EXTERNAL_ADC_CH   7

#define RTC_WRITE_ADDR        0x64
#define RTC_READ_ADDR         0x65
#define RTC_BUS_DELAY()       {u8 ticks = 15; while(ticks--);}
#define RTC_SCL_OUT()         {P3MDOUT |= 0x04; RTC_BUS_DELAY();}
#define RTC_SDA_OUT()         {P3MDOUT |= 0x08; RTC_BUS_DELAY();}
#define RTC_SDA_IN()          {P3MDOUT &= 0xF7; RTC_BUS_DELAY();}

static const u16 code ntc_table[NTC_TABLE_SIZE] = {
    32040,30490,29022,27633,26317,25071,23889,22769,21707,20700,
    19788,18838,17977,17160,16383,15646,14945,14280,13647,13045,
    12472,11928,11409,10916,10447,10000,9574,9168,8781,8413,
    8062,7727,7407,7103,6812,6534,6270,6017,5775,5545,
    5324,5114,4913,4720,4536,4360,4192,4031,3877,3730,
    3572,3454,3324,3201,3082,2968,2859,2755,2654,2558,
    2466,2378,2293,2212,2134,2059,1987,1918,1851,1788,
    1726,1668,1611,1557,1504,1454,1406,1359,1314,1271,
    1230,1190,1151,1114,1079,1045,1011,980,949,919,
    891,863,837,811,786,763,740,718,696,675
};

void ntc_init(void)
{
    ADCON = 0x80;
}

static u8 ntc_find_index(u16 r)
{
    u8 st = 0;
    u8 ed = NTC_TABLE_SIZE - 2;
    u8 mid = 0;

    if(r >= ntc_table[0]) return 0;
    if(r <= ntc_table[NTC_TABLE_SIZE - 1]) return NTC_TABLE_SIZE - 2;

    while(st <= ed)
    {
        mid = (u8)((st + ed) / 2);
        if((r <= ntc_table[mid]) && (r >= ntc_table[mid + 1])) return mid;
        if(r > ntc_table[mid])
        {
            if(mid == 0) return 0;
            ed = mid - 1;
        }
        else
        {
            st = mid + 1;
        }
    }

    return NTC_TABLE_SIZE - 2;
}

static s16 ntc_res_to_temp_x10(u16 r)
{
    u8 idx = 0;
    u16 high = 0;
    u16 low = 0;
    u16 span = 0;
    u16 pos = 0;

    idx = ntc_find_index(r);
    high = ntc_table[idx];
    low = ntc_table[idx + 1];
    span = high - low;
    if(span == 0) return (s16)idx * 10;

    pos = high - r;
    return (s16)((u16)idx * 10 + (u16)(((u32)pos * 10UL) / span));
}

static u32 ntc_adc_to_resistor(u16 adc_ntc)
{
    if(adc_ntc == 0 || adc_ntc >= NTC_ADC_FULL_SCALE) return 0;
    return ((u32)adc_ntc * NTC_PULL_RES_OHM) / (NTC_ADC_FULL_SCALE - (u32)adc_ntc);
}

s16 ntc_read_external_x10(void)
{
    u16 adc[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    u8 i = 0;
    u32 sum = 0;
    u32 r_ohm = 0;

    for(i = 0; i < NTC_SAMPLE_COUNT; i++)
    {
        sys_read_vp(NTC_ADC_VP_ADDR, (u8 *)adc, 8);
        sum += adc[NTC_EXTERNAL_ADC_CH];
    }

    r_ohm = ntc_adc_to_resistor((u16)(sum / NTC_SAMPLE_COUNT));
    if(r_ohm == 0 || r_ohm > 65535UL) return 0;

    return ntc_res_to_temp_x10((u16)r_ohm);
}

static u8 bcd_to_hex(u8 v)
{
    return (u8)(((v >> 4) * 10) + (v & 0x0F));
}

static u8 hex_to_bcd(u8 v)
{
    return (u8)(((v / 10) << 4) | (v % 10));
}

static void rtc_start(void)
{
    RTC_SDA_OUT();
    RTC_SDA_PORT = 1;
    RTC_SCL_PORT = 1;
    RTC_BUS_DELAY();
    RTC_SDA_PORT = 0;
    RTC_BUS_DELAY();
    RTC_SCL_PORT = 0;
    RTC_BUS_DELAY();
}

static void rtc_stop(void)
{
    RTC_SDA_OUT();
    RTC_SDA_PORT = 0;
    RTC_SCL_PORT = 1;
    RTC_BUS_DELAY();
    RTC_SDA_PORT = 1;
    RTC_BUS_DELAY();
    RTC_SDA_IN();
}

static u8 rtc_ack(void)
{
    u8 i = 0;

    RTC_SDA_IN();
    RTC_SDA_PORT = 1;
    RTC_BUS_DELAY();
    RTC_SCL_PORT = 1;
    RTC_BUS_DELAY();

    for(i = 0; i < 50; i++)
    {
        if(!RTC_SDA_PORT)
        {
            RTC_SCL_PORT = 0;
            RTC_BUS_DELAY();
            RTC_SDA_OUT();
            return 1;
        }
        RTC_BUS_DELAY();
    }

    RTC_SCL_PORT = 0;
    RTC_BUS_DELAY();
    RTC_SDA_OUT();
    return 0;
}

static u8 rtc_write_byte(u8 dat)
{
    u8 i = 0;

    RTC_SDA_OUT();
    for(i = 0; i < 8; i++)
    {
        RTC_SDA_PORT = (dat & 0x80) ? 1 : 0;
        dat <<= 1;
        RTC_BUS_DELAY();
        RTC_SCL_PORT = 1;
        RTC_BUS_DELAY();
        RTC_SCL_PORT = 0;
        RTC_BUS_DELAY();
    }

    return rtc_ack();
}

static u8 rtc_read_byte(void)
{
    u8 i = 0;
    u8 dat = 0;

    RTC_SDA_IN();
    for(i = 0; i < 8; i++)
    {
        RTC_BUS_DELAY();
        RTC_SCL_PORT = 1;
        RTC_BUS_DELAY();
        dat <<= 1;
        if(RTC_SDA_PORT) dat |= 0x01;
        RTC_SCL_PORT = 0;
        RTC_BUS_DELAY();
    }

    return dat;
}

static void rtc_send_ack(u8 ack)
{
    RTC_SDA_OUT();
    RTC_SDA_PORT = ack ? 0 : 1;
    RTC_BUS_DELAY();
    RTC_SCL_PORT = 1;
    RTC_BUS_DELAY();
    RTC_SCL_PORT = 0;
    RTC_BUS_DELAY();
}

static u16 rtc_write_regs(u8 reg, u8 *buf, u8 len)
{
    u8 i = 0;
    u16 status = 0;

    rtc_start();
    if(!rtc_write_byte(RTC_WRITE_ADDR)) status |= 0x0001;
    if(!rtc_write_byte(reg)) status |= 0x0002;
    for(i = 0; i < len; i++)
    {
        if(!rtc_write_byte(buf[i])) status |= 0x0004;
    }
    rtc_stop();

    return status;
}

static u16 rtc_read_regs(u8 reg, u8 *buf, u8 len)
{
    u8 i = 0;
    u16 status = 0;

    rtc_start();
    if(!rtc_write_byte(RTC_WRITE_ADDR)) status |= 0x0001;
    if(!rtc_write_byte(reg)) status |= 0x0002;
    rtc_stop();

    rtc_start();
    if(!rtc_write_byte(RTC_READ_ADDR)) status |= 0x0004;
    for(i = 0; i < len; i++)
    {
        buf[i] = rtc_read_byte();
        rtc_send_ack(i + 1 < len);
    }
    rtc_stop();

    return status;
}

static u8 rtc_time_valid(rtc_time_t *time)
{
    if(time == 0) return 0;
    if(time->year > 99) return 0;
    if(time->month == 0 || time->month > 12) return 0;
    if(time->day == 0 || time->day > 31) return 0;
    if(time->week > 7) return 0;
    if(time->hour > 23 || time->min > 59 || time->sec > 59) return 0;
    return 1;
}

u8 rtc_set_time(rtc_time_t *time)
{
    u8 reg30[1] = {0x00};
    u8 reg1c[4] = {0x48, 0x00, 0x40, 0x10};
    u8 reg1e[2] = {0x00, 0x10};
    u8 raw[7] = {0, 0, 0, 0, 0, 0, 0};
    u16 status = 0;

    if(!rtc_time_valid(time)) return SYS_ERR;

    raw[0] = hex_to_bcd(time->sec);
    raw[1] = hex_to_bcd(time->min);
    raw[2] = hex_to_bcd(time->hour);
    raw[3] = hex_to_bcd(time->week);
    raw[4] = hex_to_bcd(time->day);
    raw[5] = hex_to_bcd(time->month);
    raw[6] = hex_to_bcd(time->year);

    status = rtc_write_regs(0x30, reg30, 1);
    status |= rtc_write_regs(0x1C, reg1c, 4);
    status |= rtc_write_regs(0x10, raw, 7);
    status |= rtc_write_regs(0x1E, reg1e, 2);

    return status ? SYS_ERR : SYS_OK;
}

void rtc_init(void)
{
    u8 flag[2] = {0, 0};
    rtc_time_t default_time = {26, 5, 20, 3, 21, 10, 0, 0};

    RTC_SCL_OUT();
    RTC_SDA_OUT();
    RTC_SDA_PORT = 1;
    RTC_SCL_PORT = 1;

    if(rtc_read_regs(0x1D, flag, 2) == 0 && (flag[0] & 0x02))
    {
        rtc_set_time(&default_time);
    }
    sys_delay_ms(10);
}

u8 rtc_read(rtc_time_t *out)
{
    u8 raw[7] = {0, 0, 0, 0, 0, 0, 0};
    u16 status = 0;

    if(out == 0) return SYS_ERR;

    status = rtc_read_regs(0x10, raw, 7);

    out->sec = bcd_to_hex(raw[0] & 0x7F);
    out->min = bcd_to_hex(raw[1] & 0x7F);
    out->hour = bcd_to_hex(raw[2] & 0x3F);
    out->week = bcd_to_hex(raw[3] & 0x07);
    out->day = bcd_to_hex(raw[4] & 0x3F);
    out->month = bcd_to_hex(raw[5] & 0x1F);
    out->year = bcd_to_hex(raw[6]);

    out->fault = 0;
    if(status) out->fault |= 0x01;
    if(out->sec > 59 || out->min > 59 || out->hour > 23) out->fault |= 0x02;
    if(out->month == 0 || out->month > 12 || out->day == 0 || out->day > 31) out->fault |= 0x04;

    return (out->fault == 0) ? SYS_OK : SYS_ERR;
}
