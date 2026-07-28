#include "sys.h"
#include "ntc_rtc.h"

/* =========================
   DGUS VP ��ַ
   ========================= */

/* �û��������� */
#define VP_FAN_MODE        0x8021   // 0=�Զ���1~5=����1~5
#define VP_WORK_MODE       0x8011   // 0=���䣬1=���ȣ�2=ͨ��
#define VP_SET_TEMP        0x2233   // �趨�¶ȣ�240=24.0��

/* ��ʾ���� */
#define VP_CUR_TEMP        0x5100   // ��ǰ�¶���ʾ����λ 0.1��
#define VP_DISP_HOUR       0x6666   // Сʱ��ʾ
#define VP_DISP_MINUTE     0x6688   // ������ʾ
#define VP_DISP_SD         0x2288   // ʪ����ʾ��ĿǰĬ�� 24���ɵ���ʪ�Ȳ���

/* �������� */
#define VP_TEMP_COMP       0x0917   // �¶Ȳ�����1 = 0.1��
#define VP_HUM_COMP        0x1109   // ʪ�Ȳ�����10 = 1 ʪ��

/* ���Ա��� */
#define VP_DBG_LOOP        0x1000
#define VP_DBG_TARGET_FAN  0x8899
#define VP_DBG_PERCENT     0x8999
#define VP_DBG_FAN_MODE    0x1016
#define VP_DBG_SET_TEMP    0x1018
#define VP_DBG_WORK_MODE   0x101A
#define VP_DBG_TEMP_COMP   0x101C
#define VP_DBG_HUM_COMP    0x101E

/* ģʽ���� */
#define FAN_AUTO           0

#define MODE_COOL          0
#define MODE_HEAT          1
#define MODE_VENT          2

/* boot full action lock: reuse target_fan, no new xdata global */
#define FAN_BOOT_LOCK     99

/* =========================
   0-10V / PWM �������
   ========================= */

/*
   P1.3 = P13 / VSP
   P1.4 = P14 / PWM
*/
sbit VSP_OUT = P1^3;
sbit PWM_OUT = P1^4;

/*
   1ms tick��20 tick һ������ = 50Hz
   fan_percent:
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
   RTC һ����Уʱ����
   ========================= */

/*
   ��ҪУʱʱ��
   1. �� RTC_FORCE_SET_ONCE �ĳ� 1
   2. �޸� rtc_set_time_once_if_needed() ���ʱ��
   3. ��¼һ��
   4. ʱ������Ժ󣬱���Ļ� 0
   5. ����һ����ʽ��

   ��ʽ����ʱ���뱣�� 0��
*/
#define RTC_FORCE_SET_ONCE  0

/* =========================
   �ϵ�̶���ʼ����
   ========================= */

typedef struct
{
    u16 addr;
    s16 value;
} vp_init_item_t;

code vp_init_item_t vp_init_table[] =
{
    /* д 1 */
    {0x3277, 1},
    {0x1041, 1},

    /* д 0 */
    {0x1833, 0},
    {0x1835, 0},
    {0x1843, 0},
    {0x8226, 0},

    /* ����Ĭ��ֵ��ֻ�ϵ�дһ�� */
    {0x0917, 0},
    {0x1109, 0},

    /* д 100 */
    {0x1075, 100},

    /* �ض��̶�ֵ */
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
   ȫ�ֱ���
   ������ xdata������ DATA/IDATA ��
   ========================= */

xdata rtc_time_t rtc;

xdata u16 loop_count;

xdata s16 cur_temp_raw_x10;    // ������ԭʼ�¶�
xdata s16 cur_temp_show_x10;   // ���������ʾ�¶�
xdata s16 set_temp_x10;        // �趨�¶�

xdata s16 temp_comp_x10;       // �¶Ȳ�����1 = 0.1��
xdata s16 hum_comp_x10;        // ʪ�Ȳ�����10 = 1 ʪ��

xdata s16 hum_raw;             // ԭʼʪ�ȣ�Ŀǰ�̶� 24
xdata s16 hum_show;            // ���������ʾʪ��

xdata u16 fan_mode;
xdata u16 work_mode;

xdata u16 target_fan;
xdata u16 fan_percent;

xdata u16 disp_hour;
xdata u16 disp_minute;

/* =========================
   ��ʼ������
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
   0-10V / PWM ���
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
       P1.3 / P1.4 �������
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
       �� 0~100% ����� 0~20 tick
       20% -> 4 tick
       40% -> 8 tick
       60% -> 12 tick
       80% -> 16 tick
       100% -> 20 tick
    */
    output_duty_ticks = (u8)((percent * OUTPUT_PWM_PERIOD_TICKS) / 100);
}

/*
   ��������� sys.c �� Timer2 �ж�ÿ 1ms ��һ�Ρ�
   ����д�� static����Ϊ sys.c Ҫ extern ��������
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
   RTC һ����Уʱ
   ========================= */

static void rtc_set_time_once_if_needed(void)
{
#if RTC_FORCE_SET_ONCE
    rtc_time_t set_time;

    /*
       ʾ����2026-07-19 18:41:00

       �ṹ��˳��
       year, month, day, week, hour, min, sec, fault

       week ��� UI ����ʾ���ڣ�Ӱ�첻��
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
   �ϵ�̶�д VP
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
   �����߼�
   ========================= */

static u16 fan_to_percent(u16 fan)
{
    /*
       switch-valve mode:
       keep the old function name, but now it returns action duration seconds.
       1=low 6s, 2=mid 11s, 3=high 16s, 4/5=full 20s.
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

    if(mode == MODE_COOL)
    {
        diff = cur_temp - set_temp;
    }
    else if(mode == MODE_HEAT)
    {
        diff = set_temp - cur_temp;
    }
    else
    {
        return 4;   /* vent/default: full */
    }

    /*
       switch-valve auto mode with 2.0C hysteresis.
       target_fan is reused as the previous/last executed level.
       No new global/xdata variable is added.

       Rising/load increasing:
       diff > 3.0C -> low
       diff > 1.0C -> mid
       otherwise   -> full

       Falling/load decreasing with 2.0C hysteresis:
       low exits when diff <= 1.0C
       mid exits to full when diff <= -1.0C
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
   ��ǰ�˱���
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
   ��������
   ========================= */

static void protect_user_vars(void)
{
    /* ���ٱ�����0=�Զ���1~5=�ֶ����� */
    if(fan_mode > 5)
    {
        fan_mode = FAN_AUTO;
        sys_write_vp(VP_FAN_MODE, (u8 *)&fan_mode, 1);
    }

    /* ģʽ������0=���䣬1=���ȣ�2=ͨ�� */
    if(work_mode > 2)
    {
        work_mode = MODE_COOL;
        sys_write_vp(VP_WORK_MODE, (u8 *)&work_mode, 1);
    }

    /*
       �趨�¶ȱ�����
       10.0�� ~ 35.0��
       ���޿��߽磬���� 24.0��
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
       �¶Ȳ���������
       -10.0�� ~ +10.0��
       Ҳ���� -100 ~ +100
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
       ʪ�Ȳ���������
       -50 ~ +50 ʪ�ȡ�
       ��Ϊ 10 = 1 ʪ�ȣ����Է�Χ�� -500 ~ +500��
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
   ��ʪ����ʾ
   ========================= */

static void update_sensor_display_values(void)
{
    /* ��ȡ��ʵ�¶� */
    cur_temp_raw_x10 = ntc_read_external_x10();

    /*
       �¶���ʾ = ������ԭʼ�¶� + �¶Ȳ���
       �¶Ȳ�����λ��1 = 0.1��
    */
    cur_temp_show_x10 = cur_temp_raw_x10 + temp_comp_x10;

    /*
       ʪ��Ŀǰû����ʵ�����������ù̶� 24��
       �����������ʵʪ�ȴ��������� hum_raw = 24 �滻����ʵ������
    */
    hum_raw = 24;

    /*
       ʪ����ʾ = ԭʼʪ�� + ʪ�Ȳ��� / 10
       ʪ�Ȳ�����λ��10 = 1 ʪ��
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
   RTC ��ʾ
   ========================= */

static void update_rtc_display_values(void)
{
    rtc_read(&rtc);

    disp_hour = rtc.hour;
    disp_minute = rtc.min;
}

/* =========================
   ���ٸ���
   ========================= */

static void update_fan_logic(void)
{
    u16 new_target_fan;
    u16 duration;

    /*
       Boot action lock:
       After power-on, force full output for 20s first.
       During these 20s, do NOT let VP_FAN_MODE or auto temperature logic
       change it into low/manual-1.
       No new global/xdata variable is used; target_fan == FAN_BOOT_LOCK
       means the boot full action is still running.
    */
    if(target_fan == FAN_BOOT_LOCK)
    {
        if(fan_percent > 0)
        {
            fan_percent--;

            if(fan_percent == 0)
            {
                output_set_percent(0);
                target_fan = 0;
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
       IMPORTANT:
       No new global/xdata variables are used in this build.
       target_fan  = last executed fan level
       fan_percent = remaining output seconds, for debug display
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

/* =========================
   д��ʾ
   ========================= */

static void write_display_values(void)
{
    sys_write_vp(VP_CUR_TEMP, (u8 *)&cur_temp_show_x10, 1);
    sys_write_vp(VP_DISP_HOUR, (u8 *)&disp_hour, 1);
    sys_write_vp(VP_DISP_MINUTE, (u8 *)&disp_minute, 1);
    sys_write_vp(VP_DISP_SD, (u8 *)&hum_show, 1);
}

/* =========================
   д����
   ========================= */

static void write_debug_values(void)
{
    u16 dbg_target_fan;

    if(target_fan == FAN_BOOT_LOCK)
    {
        dbg_target_fan = 4;
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
    sys_init();
    app_vars_init();

    /*
       ��ʼ�� 0-10V/PWM �����
       ��������ѭ��ǰִ�С�
    */
    output_init();

    ntc_init();
    rtc_init();
    rtc_set_time_once_if_needed();

    /*
       �ϵ��ʼ����
       ��Щ��ַֻдһ�Σ���Ҫ�� while �ﷴ��д��
       ����ǰ�˵����¶Ȳ���/ʪ�Ȳ����ᱻ��̨�����
    */
    vp_write_init_table();

    /*
       �ϵ�Ĭ��ֵ��
       ע�⣺��ЩҲ��ֻдһ�Ρ�
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
       Switch-valve boot action:
       power on -> full level, output 100% for 20s, then update_fan_logic() will stop it.
       target_fan/fan_percent are existing global variables, no new xdata variables are added.
    */
    target_fan = FAN_BOOT_LOCK;
    fan_percent = 20;
    output_set_percent(100);

    while(1)
    {
        loop_count++;

        /*
           ˳��
           1. ��ǰ�˱���
           2. ������
           3. �������������Ӳ���
           4. ��ʱ��
           5. �����
           6. ���� 0-10V/PWM ���
           7. д����ʾ
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