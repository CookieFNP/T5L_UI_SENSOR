#include "sys.h"
#include "ntc_rtc.h"

/* =========================
   DGUS VP 地址
   ========================= */

/* 用户操作变量 */
#define VP_FAN_MODE        0x8021   // 0=自动，1~5=风速1~5
#define VP_WORK_MODE       0x8011   // 0=制冷，1=制热，2=通风
#define VP_SET_TEMP        0x2233   // 设定温度，240=24.0℃

/* 显示变量 */
#define VP_CUR_TEMP        0x5100   // 当前温度显示，单位 0.1℃
#define VP_DISP_HOUR       0x6666   // 小时显示
#define VP_DISP_MINUTE     0x6688   // 分钟显示
#define VP_DISP_SD         0x2288   // 湿度显示，目前默认 24，可叠加湿度补偿

/* 补偿变量 */
#define VP_TEMP_COMP       0x0917   // 温度补偿，1 = 0.1℃
#define VP_HUM_COMP        0x1109   // 湿度补偿，10 = 1 湿度

/* 调试变量，正式 UI 没显示也没关系 */
#define VP_DBG_LOOP        0x1000
#define VP_DBG_TARGET_FAN  0x8899
#define VP_DBG_PERCENT     0x8999
#define VP_DBG_FAN_MODE    0x1016
#define VP_DBG_SET_TEMP    0x1018
#define VP_DBG_WORK_MODE   0x101A
#define VP_DBG_TEMP_COMP   0x101C
#define VP_DBG_HUM_COMP    0x101E

/* 模式定义 */
#define FAN_AUTO           0

#define MODE_COOL          0
#define MODE_HEAT          1
#define MODE_VENT          2

/* =========================
   RTC 一次性校时开关
   ========================= */

/*
   需要校时时：
   1. 把 RTC_FORCE_SET_ONCE 改成 1
   2. 修改下面 rtc_set_time_once_if_needed() 里的时间
   3. 烧录一次
   4. 看到时间对了以后，必须改回 0
   5. 再烧一次正式版

   正式运行时必须保持 0，否则每次上电都会回到写死的时间。
*/
#define RTC_FORCE_SET_ONCE  0

/* =========================
   上电固定初始化表
   ========================= */

typedef struct
{
    u16 addr;
    s16 value;
} vp_init_item_t;

code vp_init_item_t vp_init_table[] =
{
    /* 写 1 */
    {0x3277, 1},
    {0x1041, 1},

    /* 写 0 */
    {0x1833, 0},
    {0x1835, 0},
    {0x1843, 0},
    {0x8226, 0},

    /* 补偿默认值：只上电写一次 */
    {0x0917, 0},
    {0x1109, 0},

    /* 写 100 */
    {0x1075, 100},

    /* 特定固定值 */
    {0x1099, 10},
    {0x1059, 30},
    {0x1061, 10},
    {0x1063, 25},
    {0x1065, 50},
    {0x1067, 70},
    {0x1069, 100},
	{0x1043,150},
	{0x1045,300},
	{0x1047,170},
	{0x1049,320},
};

#define VP_INIT_TABLE_SIZE  (sizeof(vp_init_table) / sizeof(vp_init_table[0]))

/* =========================
   全局变量
   尽量放 xdata，避免 DATA/IDATA 爆
   ========================= */

xdata rtc_time_t rtc;

xdata u16 loop_count;

xdata s16 cur_temp_raw_x10;    // 传感器原始温度
xdata s16 cur_temp_show_x10;   // 补偿后的显示温度
xdata s16 set_temp_x10;        // 设定温度

xdata s16 temp_comp_x10;       // 温度补偿，1 = 0.1℃
xdata s16 hum_comp_x10;        // 湿度补偿，10 = 1 湿度

xdata s16 hum_raw;             // 原始湿度，目前固定 24
xdata s16 hum_show;            // 补偿后的显示湿度

xdata u16 fan_mode;
xdata u16 work_mode;

xdata u16 target_fan;
xdata u16 fan_percent;

xdata u16 disp_hour;
xdata u16 disp_minute;

/* =========================
   初始化变量
   ========================= */

static void app_vars_init(void)
{
    loop_count = 0;

    cur_temp_raw_x10 = 0;
    cur_temp_show_x10 = 0;
    set_temp_x10 = 240;

    temp_comp_x10 = 0;
    hum_comp_x10 = 0;

    hum_raw = 24;
    hum_show = 24;

    fan_mode = FAN_AUTO;
    work_mode = MODE_COOL;

    target_fan = 1;
    fan_percent = 20;

    disp_hour = 0;
    disp_minute = 0;
}

/* =========================
   RTC 一次性校时
   ========================= */

static void rtc_set_time_once_if_needed(void)
{
#if RTC_FORCE_SET_ONCE
    rtc_time_t set_time;

    /*
       这里是校时时间。
       当前示例：2026-07-19 18:41:00

       结构体顺序：
       year, month, day, week, hour, min, sec, fault

       week 如果 UI 不显示星期，影响不大。
       2026-07-19 是周日，这里填 7。
    */
    set_time.year  = 26;
    set_time.month = 7;
    set_time.day   = 19;
    set_time.week  = 7;
    set_time.hour  = 20;
    set_time.min   = 02;
    set_time.sec   = 0;
    set_time.fault = 0;

    rtc_set_time(&set_time);
    sys_delay_ms(50);
#endif
}

/* =========================
   上电固定写 VP
   ========================= */

static void vp_write_init_table(void)
{
    u8 i;
    s16 value;

    for(i = 0; i < VP_INIT_TABLE_SIZE; i++)
    {
        value = vp_init_table[i].value;
        sys_write_vp(vp_init_table[i].addr, (u8 *)&value, 1);
        sys_delay_ms(5);
    }
}

/* =========================
   风速逻辑
   ========================= */

static u16 fan_to_percent(u16 fan)
{
    switch(fan)
    {
        case 1: return 20;
        case 2: return 40;
        case 3: return 60;
        case 4: return 80;
        case 5: return 100;
        default: return 20;
    }
}

static u16 calc_auto_fan(u16 mode, s16 cur_temp, s16 set_temp)
{
    s16 diff = 0;

    if(mode == MODE_COOL)
    {
        /* 制冷：当前温度高于设定温度，风速变大 */
        diff = cur_temp - set_temp;
    }
    else if(mode == MODE_HEAT)
    {
        /* 制热：设定温度高于当前温度，风速变大 */
        diff = set_temp - cur_temp;
    }
    else
    {
        /* 通风：自动时默认一档 */
        return 1;
    }

    /*
       温度单位是 x10：
       10 = 1.0℃
       30 = 3.0℃
    */
    if(diff < 10)
    {
        return 1;
    }
    else if(diff < 30)
    {
        return 2;
    }
    else
    {
        return 3;
    }
}

static u16 calc_target_fan(u16 fan_sel, u16 mode, s16 cur_temp, s16 set_temp)
{
    if(fan_sel >= 1 && fan_sel <= 5)
    {
        return fan_sel;
    }

    return calc_auto_fan(mode, cur_temp, set_temp);
}

/* =========================
   读前端变量
   ========================= */

static void read_user_vars(void)
{
    sys_read_vp(VP_FAN_MODE, (u8 *)&fan_mode, 1);
    sys_read_vp(VP_WORK_MODE, (u8 *)&work_mode, 1);
    sys_read_vp(VP_SET_TEMP, (u8 *)&set_temp_x10, 1);

    sys_read_vp(VP_TEMP_COMP, (u8 *)&temp_comp_x10, 1);
    sys_read_vp(VP_HUM_COMP, (u8 *)&hum_comp_x10, 1);
}

/* =========================
   变量保护
   ========================= */

static void protect_user_vars(void)
{
    /* 风速保护：0=自动，1~5=手动风速 */
    if(fan_mode > 5)
    {
        fan_mode = FAN_AUTO;
        sys_write_vp(VP_FAN_MODE, (u8 *)&fan_mode, 1);
    }

    /* 模式保护：0=制冷，1=制热，2=通风 */
    if(work_mode > 2)
    {
        work_mode = MODE_COOL;
        sys_write_vp(VP_WORK_MODE, (u8 *)&work_mode, 1);
    }

    /*
       设定温度保护：
       10.0℃ ~ 35.0℃
       超限卡边界，不回 24.0℃
    */
    if(set_temp_x10 < 100)
    {
        set_temp_x10 = 100;
        sys_write_vp(VP_SET_TEMP, (u8 *)&set_temp_x10, 1);
    }
    else if(set_temp_x10 > 350)
    {
        set_temp_x10 = 350;
        sys_write_vp(VP_SET_TEMP, (u8 *)&set_temp_x10, 1);
    }

    /*
       温度补偿保护：
       -10.0℃ ~ +10.0℃
       也就是 -100 ~ +100
    */
    if(temp_comp_x10 < -100)
    {
        temp_comp_x10 = -100;
        sys_write_vp(VP_TEMP_COMP, (u8 *)&temp_comp_x10, 1);
    }
    else if(temp_comp_x10 > 100)
    {
        temp_comp_x10 = 100;
        sys_write_vp(VP_TEMP_COMP, (u8 *)&temp_comp_x10, 1);
    }

    /*
       湿度补偿保护：
       -50 ~ +50 湿度。
       因为 10 = 1 湿度，所以范围是 -500 ~ +500。
    */
    if(hum_comp_x10 < -500)
    {
        hum_comp_x10 = -500;
        sys_write_vp(VP_HUM_COMP, (u8 *)&hum_comp_x10, 1);
    }
    else if(hum_comp_x10 > 500)
    {
        hum_comp_x10 = 500;
        sys_write_vp(VP_HUM_COMP, (u8 *)&hum_comp_x10, 1);
    }
}

/* =========================
   温湿度显示
   ========================= */

static void update_sensor_display_values(void)
{
    /* 读取真实温度 */
    cur_temp_raw_x10 = ntc_read_external_x10();

    /*
       温度显示 = 传感器原始温度 + 温度补偿
       温度补偿单位：1 = 0.1℃
    */
    cur_temp_show_x10 = cur_temp_raw_x10 + temp_comp_x10;

    /*
       湿度目前没有真实传感器，先用固定 24。
       后续如果有真实湿度传感器，把 hum_raw = 24 替换成真实读数。
    */
    hum_raw = 24;

    /*
       湿度显示 = 原始湿度 + 湿度补偿 / 10
       湿度补偿单位：10 = 1 湿度
    */
    hum_show = hum_raw + hum_comp_x10 / 10;

    if(hum_show < 0)
    {
        hum_show = 0;
    }
    else if(hum_show > 100)
    {
        hum_show = 100;
    }
}

/* =========================
   RTC 显示
   ========================= */

static void update_rtc_display_values(void)
{
    rtc_read(&rtc);

    disp_hour = rtc.hour;
    disp_minute = rtc.min;
}

/* =========================
   风速更新
   ========================= */

static void update_fan_logic(void)
{
    /*
       自动风速使用“补偿后的显示温度”参与比较。
    */
    target_fan = calc_target_fan(
        fan_mode,
        work_mode,
        cur_temp_show_x10,
        set_temp_x10
    );

    fan_percent = fan_to_percent(target_fan);
}

/* =========================
   写显示
   ========================= */

static void write_display_values(void)
{
    sys_write_vp(VP_CUR_TEMP, (u8 *)&cur_temp_show_x10, 1);
    sys_write_vp(VP_DISP_HOUR, (u8 *)&disp_hour, 1);
    sys_write_vp(VP_DISP_MINUTE, (u8 *)&disp_minute, 1);
    sys_write_vp(VP_DISP_SD, (u8 *)&hum_show, 1);
}

/* =========================
   写调试
   ========================= */

static void write_debug_values(void)
{
    sys_write_vp(VP_DBG_LOOP, (u8 *)&loop_count, 1);
    sys_write_vp(VP_DBG_TARGET_FAN, (u8 *)&target_fan, 1);
    sys_write_vp(VP_DBG_PERCENT, (u8 *)&fan_percent, 1);
    sys_write_vp(VP_DBG_FAN_MODE, (u8 *)&fan_mode, 1);
    sys_write_vp(VP_DBG_SET_TEMP, (u8 *)&set_temp_x10, 1);
    sys_write_vp(VP_DBG_WORK_MODE, (u8 *)&work_mode, 1);
    sys_write_vp(VP_DBG_TEMP_COMP, (u8 *)&temp_comp_x10, 1);
    sys_write_vp(VP_DBG_HUM_COMP, (u8 *)&hum_comp_x10, 1);
}

/* =========================
   main
   ========================= */

int main(void)
{
    sys_init();
    app_vars_init();

    ntc_init();
    rtc_init();
    rtc_set_time_once_if_needed();

    /*
       上电初始化：
       这些地址只写一次，不要在 while 里反复写。
       否则前端调节温度补偿/湿度补偿会被后台清掉。
    */
    vp_write_init_table();

    /*
       上电默认值。
       注意：这些也是只写一次。
    */
    set_temp_x10 = 240;
    fan_mode = FAN_AUTO;
    work_mode = MODE_COOL;
    hum_show = 58;

    sys_write_vp(VP_SET_TEMP, (u8 *)&set_temp_x10, 1);
    sys_write_vp(VP_FAN_MODE, (u8 *)&fan_mode, 1);
    sys_write_vp(VP_WORK_MODE, (u8 *)&work_mode, 1);
    sys_write_vp(VP_DISP_SD, (u8 *)&hum_show, 1);

    while(1)
    {
        loop_count++;

        /*
           顺序：
           1. 读前端变量
           2. 做保护
           3. 读传感器并叠加补偿
           4. 读时钟
           5. 算风速
           6. 写回显示
        */
        read_user_vars();
        protect_user_vars();

        update_sensor_display_values();
        update_rtc_display_values();

        update_fan_logic();

        write_display_values();
        write_debug_values();

        sys_delay_ms(1000);
    }
}