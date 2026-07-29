#include "sys.h"
#include "ntc_rtc.h"

/* =========================
   DGUS VP 地址
   ========================= */

/* 用户设置变量 */
#define VP_FAN_MODE        0x8021   // 0=自动，1~5=手动风速1~5
#define VP_WORK_MODE       0x8011   // 0=制冷，1=制热，2=通风
#define VP_SET_TEMP        0x2233   // 设定温度，240=24.0℃
#define VP_POWER_STATE     0x7373   // 前端开关机状态：0=关机/待机，1=开机/运行

/* 显示变量 */
#define VP_CUR_TEMP        0x5100   // 当前温度显示，单位 0.1℃
#define VP_DISP_HOUR       0x6666   // 小时显示
#define VP_DISP_MINUTE     0x6688   // 分钟显示
#define VP_DISP_SD         0x2288   // 湿度显示，目前默认 24，可叠加湿度补偿

/* 补偿变量 */
#define VP_TEMP_COMP       0x0917   // 温度补偿，1 = 0.1℃
#define VP_HUM_COMP        0x1109   // 湿度补偿，10 = 1 湿度

/* 调试变量 */
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

/* 特殊状态：复用 target_fan，不额外增加太多全局变量 */
#define FAN_POWER_OFF      98       // 关机/待机状态
#define FAN_BOOT_LOCK      99       // 开机满档动作锁定状态

/* =========================
   0-10V / PWM 输出设置
   ========================= */

/*
   P1.3 = P13 / VSP
   P1.4 = P14 / PWM
*/
sbit VSP_OUT = P1^3;
sbit PWM_OUT = P1^4;

/*
   1ms tick，20 tick 一个周期 = 50Hz
   输出百分比：
   0   -> 0V
   20  -> 2V
   40  -> 4V
   60  -> 6V
   80  -> 8V
   100 -> 10V
*/
#define OUTPUT_PWM_PERIOD_TICKS  20

xdata u8 output_pwm_tick;
xdata u8 output_duty_ticks;
xdata u16 output_percent_now;

/* =========================
   RTC 一次性校时开关
   ========================= */

/*
   需要校时时：
   1. 把 RTC_FORCE_SET_ONCE 改成 1
   2. 修改 rtc_set_time_once_if_needed() 里面的时间
   3. 烧录一次
   4. 时间设置以后，必须改回 0
   5. 再烧录正式版

   正式运行时必须保持 0。
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

    /* 补偿默认值，只上电写一次 */
    {0x0917, 0},
    {0x1109, 0},

    /* 写 100 */
    {0x1075, 100},

    /* 其他固定默认值 */
    {0x1099, 10},
    {0x1059, 30},
    {0x1061, 10},
    {0x1063, 25},
    {0x1065, 50},
    {0x1067, 70},
    {0x1069, 100},
    {0x1043, 150},
    {0x1045, 300},
    {0x1047, 170},
    {0x1049, 320},
};

#define VP_INIT_TABLE_SIZE  (sizeof(vp_init_table) / sizeof(vp_init_table[0]))

/* =========================
   全局变量
   尽量放 xdata，避免 DATA/IDATA 不够
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

    /* 通电默认待机，不执行风阀动作 */
    target_fan = FAN_POWER_OFF;
    fan_percent = 0;

    disp_hour = 0;
    disp_minute = 0;
}

/* =========================
   0-10V / PWM 输出
   ========================= */

static void output_pin_set(u8 level)
{
    if(level)
    {
        VSP_OUT = 1;
        PWM_OUT = 1;
    }
    else
    {
        VSP_OUT = 0;
        PWM_OUT = 0;
    }
}

static void output_init(void)
{
    /*
       P1.3 / P1.4 设置输出
       bit3 = 0x08
       bit4 = 0x10
    */
    P1MDOUT |= 0x18;

    output_pwm_tick = 0;
    output_duty_ticks = 0;
    output_percent_now = 0;

    output_pin_set(0);
}

static void output_set_percent(u16 percent)
{
    if(percent > 100)
    {
        percent = 100;
    }

    output_percent_now = percent;

    /*
       把 0~100% 映射到 0~20 tick
       20% -> 4 tick
       40% -> 8 tick
       60% -> 12 tick
       80% -> 16 tick
       100% -> 20 tick
    */
    output_duty_ticks = (u8)((percent * OUTPUT_PWM_PERIOD_TICKS) / 100);
}

/*
   这个函数由 sys.c 的 Timer2 中断每 1ms 调用一次。
   不能写成 static，因为 sys.c 要 extern 调用它。
*/
void output_pwm_tick_1ms(void)
{
    if(output_duty_ticks == 0)
    {
        output_pin_set(0);
    }
    else if(output_duty_ticks >= OUTPUT_PWM_PERIOD_TICKS)
    {
        output_pin_set(1);
    }
    else
    {
        if(output_pwm_tick < output_duty_ticks)
        {
            output_pin_set(1);
        }
        else
        {
            output_pin_set(0);
        }
    }

    output_pwm_tick++;

    if(output_pwm_tick >= OUTPUT_PWM_PERIOD_TICKS)
    {
        output_pwm_tick = 0;
    }
}

/* =========================
   RTC 一次性校时
   ========================= */

static void rtc_set_time_once_if_needed(void)
{
#if RTC_FORCE_SET_ONCE
    rtc_time_t set_time;

    /*
       示例：2026-07-19 18:41:00

       结构体顺序：
       year, month, day, week, hour, min, sec, fault

       week 只影响 UI 显示星期，影响不大
    */
    set_time.year  = 26;
    set_time.month = 7;
    set_time.day   = 19;
    set_time.week  = 7;
    set_time.hour  = 18;
    set_time.min   = 41;
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
   风阀档位逻辑
   ========================= */

static u16 fan_to_percent(u16 fan)
{
    /*
       开关阀定时动作：这里保留旧函数名，但返回值现在表示动作持续秒数。
       1=低档 6s，2=中档 11s，3=高档 16s，4/5=满档 20s。
    */
    switch(fan)
    {
        case 1: return 6;
        case 2: return 11;
        case 3: return 16;
        case 4: return 20;
        case 5: return 20;
        default: return 20;
    }
}

static u16 calc_auto_fan(u16 mode, s16 cur_temp, s16 set_temp)
{
    s16 diff = 0;

    /*
       按甲方逻辑：
       制冷：房间过冷才调小，所以看 set_temp - cur_temp。
       制热：房间过热才调小，所以看 cur_temp - set_temp。

       diff > 3.0℃ -> 低档
       diff > 1.0℃ -> 中档
       否则        -> 满档
    */
    if(mode == MODE_COOL)
    {
        diff = set_temp - cur_temp;
    }
    else if(mode == MODE_HEAT)
    {
        diff = cur_temp - set_temp;
    }
    else
    {
        return 4;   /* 通风默认满档 */
    }

    /*
       带 2.0℃ 回差。
       target_fan 复用为上一次执行/保持的档位，不再新增全局变量。

       当前低档：diff 降到 <= 1.0℃ 时，退出低档到中档。
       当前中档：diff > 3.0℃ 进低档；diff <= -1.0℃ 回满档。
       当前满档/其他：按阈值直接判断。
    */
    if(target_fan == 1)
    {
        if(diff <= 10)
        {
            return 2;
        }
        return 1;
    }
    else if(target_fan == 2)
    {
        if(diff > 30)
        {
            return 1;
        }
        else if(diff <= -10)
        {
            return 4;
        }
        return 2;
    }
    else
    {
        if(diff > 30)
        {
            return 1;
        }
        else if(diff > 10)
        {
            return 2;
        }
        return 4;
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

static u16 read_power_state_protected(void)
{
    u16 pwr;

    sys_read_vp(VP_POWER_STATE, (u8 *)&pwr, 1);

    if(pwr > 1)
    {
        pwr = 0;
        sys_write_vp(VP_POWER_STATE, (u8 *)&pwr, 1);
    }

    return pwr;
}

/* =========================
   参数保护
   ========================= */

static void protect_user_vars(void)
{
    /* 风速变量：0=自动，1~5=手动风速 */
    if(fan_mode > 5)
    {
        fan_mode = FAN_AUTO;
        sys_write_vp(VP_FAN_MODE, (u8 *)&fan_mode, 1);
    }

    /* 工作模式：0=制冷，1=制热，2=通风 */
    if(work_mode > 2)
    {
        work_mode = MODE_COOL;
        sys_write_vp(VP_WORK_MODE, (u8 *)&work_mode, 1);
    }

    /*
       设定温度保护：
       10.0℃ ~ 35.0℃
       超界拉回边界
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
       后续如果接真实湿度传感器，用真实值替换 hum_raw = 24。
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
   风阀更新
   ========================= */

static void update_fan_logic(void)
{
    u16 new_target_fan;
    u16 duration;

    /*
       开机满档锁定：
       点击开机后，先强制满档输出 20s。
       这 20s 内不允许 VP_FAN_MODE 或自动温控逻辑把它改成低档/手动1。
    */
    if(target_fan == FAN_BOOT_LOCK)
    {
        if(fan_percent > 0)
        {
            fan_percent--;

            if(fan_percent == 0)
            {
                output_set_percent(0);

                /* 满档动作已经执行过，记录当前已经是满档，避免下一轮自动满档时立刻重复执行 */
                target_fan = 4;
            }
        }
        return;
    }

    new_target_fan = calc_target_fan(
        fan_mode,
        work_mode,
        cur_temp_show_x10,
        set_temp_x10
    );

    /*
       target_fan  = 上一次执行/保持的档位
       fan_percent = 剩余输出秒数，用于调试显示
    */
    if(new_target_fan != target_fan)
    {
        target_fan = new_target_fan;
        duration = fan_to_percent(target_fan);
        fan_percent = duration;

        if(duration > 0)
        {
            output_set_percent(100);
        }
        else
        {
            output_set_percent(0);
        }
    }
    else
    {
        if(fan_percent > 0)
        {
            fan_percent--;

            if(fan_percent == 0)
            {
                output_set_percent(0);
            }
        }
    }
}

static void update_power_and_fan_logic(void)
{
    u16 pwr;

    pwr = read_power_state_protected();

    /*
       关机/待机：
       显示、温湿度、RTC 继续刷新，但风阀输出必须保持 0V。
    */
    if(pwr == 0)
    {
        if(target_fan != FAN_POWER_OFF)
        {
            output_set_percent(0);
            target_fan = FAN_POWER_OFF;
            fan_percent = 0;
        }
        else
        {
            output_set_percent(0);
        }
        return;
    }

    /*
       开机边沿：
       前端把 0x7373 从 0 写成 1 后，target_fan 仍是 FAN_POWER_OFF，
       这里触发一次“开机满档动作”。
    */
    if(target_fan == FAN_POWER_OFF)
    {
        fan_mode = FAN_AUTO;
        sys_write_vp(VP_FAN_MODE, (u8 *)&fan_mode, 1);

        target_fan = FAN_BOOT_LOCK;
        fan_percent = 20;
        output_set_percent(100);
        return;
    }

    update_fan_logic();
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
    u16 dbg_target_fan;

    if(target_fan == FAN_BOOT_LOCK)
    {
        dbg_target_fan = 4;
    }
    else if(target_fan == FAN_POWER_OFF)
    {
        dbg_target_fan = 0;
    }
    else
    {
        dbg_target_fan = target_fan;
    }

    sys_write_vp(VP_DBG_LOOP, (u8 *)&loop_count, 1);
    sys_write_vp(VP_DBG_TARGET_FAN, (u8 *)&dbg_target_fan, 1);
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
    u16 pwr_init;

    sys_init();
    app_vars_init();

    /* 初始化 0-10V/PWM 输出，一定要在主循环前执行 */
    output_init();

    ntc_init();
    rtc_init();
    rtc_set_time_once_if_needed();

    /*
       上电初始化表：
       这些地址只写一次，不要放到 while 里反复写。
    */
    vp_write_init_table();

    /*
       上电默认值：
       注意：这些也只写一次。
    */
    set_temp_x10 = 240;
    fan_mode = FAN_AUTO;
    work_mode = MODE_COOL;
    hum_show = 24;

    sys_write_vp(VP_SET_TEMP, (u8 *)&set_temp_x10, 1);
    sys_write_vp(VP_FAN_MODE, (u8 *)&fan_mode, 1);
    sys_write_vp(VP_WORK_MODE, (u8 *)&work_mode, 1);
    sys_write_vp(VP_DISP_SD, (u8 *)&hum_show, 1);

    /*
       通电待机：
       通电后不执行满档动作。
       前端开机按钮会把 0x7373 写成 1；
       前端关机按钮会把 0x7373 写成 0；
       C 只读取 0x7373，不负责切页面。
    */
    pwr_init = 0;
    sys_write_vp(VP_POWER_STATE, (u8 *)&pwr_init, 1);

    target_fan = FAN_POWER_OFF;
    fan_percent = 0;
    output_set_percent(0);

    while(1)
    {
        loop_count++;

        /*
           顺序：
           1. 读前端变量
           2. 参数保护
           3. 更新传感器显示值
           4. 更新时间
           5. 根据 0x7373 决定是否执行风阀逻辑
           6. 写显示
           7. 写调试
        */
        read_user_vars();
        protect_user_vars();

        update_sensor_display_values();
        update_rtc_display_values();

        update_power_and_fan_logic();

        write_display_values();
        write_debug_values();

        sys_delay_ms(1000);
    }
}
