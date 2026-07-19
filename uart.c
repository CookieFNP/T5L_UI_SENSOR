#include "uart.h"

sbit UART4_DIR = P0^0;
sbit UART5_DIR = P0^1;

#define UART_TX_TIMEOUT 60000UL

static u32 uart_state = 0;
static u32 uart4_bytes = 0;
static u32 uart5_bytes = 0;
static u16 uart4_frames = 0;
static u16 uart5_frames = 0;

static void uart_vars_init(void)
{
    uart_state = 0;
    uart4_bytes = 0;
    uart5_bytes = 0;
    uart4_frames = 0;
    uart5_frames = 0;
}

static void uart_dir_delay(void)
{
    u8 i = 0;
    for(i = 0; i < 20; i++);
}

void uart4_init(void)
{
    P0MDOUT |= 0x01;
    UART4_DIR = 0;

    SCON2T = 0x80;
    SCON2R = 0x80;
    BODE2_DIV_H = 0x00;
    BODE2_DIV_L = 0xE0;

    ES2R = 0;
    ES2T = 0;
}

void uart5_init(void)
{
    P0MDOUT |= 0x02;
    UART5_DIR = 0;

    SCON3T = 0x80;
    SCON3R = 0x80;
    BODE3_DIV_H = 0x00;
    BODE3_DIV_L = 0xE0;

    ES3R = 0;
    ES3T = 0;
}

void uart_init(void)
{
    uart_vars_init();
    uart4_init();
    uart5_init();
}

u32 uart_status(void)
{
    return uart_state;
}

u32 uart4_tx_bytes(void)
{
    return uart4_bytes;
}

u32 uart5_tx_bytes(void)
{
    return uart5_bytes;
}

u16 uart4_tx_frames(void)
{
    return uart4_frames;
}

u16 uart5_tx_frames(void)
{
    return uart5_frames;
}

u8 uart4_send_byte(u8 dat)
{
    u32 timeout = UART_TX_TIMEOUT;

    SBUF2_TX = dat;
    while((SCON2T & 0x01) == 0)
    {
        if(timeout-- == 0)
        {
            uart_state |= 0x00000001UL;
            return SYS_ERR;
        }
    }
    SCON2T &= 0xFE;
    uart4_bytes++;
    return SYS_OK;
}

u8 uart5_send_byte(u8 dat)
{
    u32 timeout = UART_TX_TIMEOUT;

    SBUF3_TX = dat;
    while((SCON3T & 0x01) == 0)
    {
        if(timeout-- == 0)
        {
            uart_state |= 0x00000002UL;
            return SYS_ERR;
        }
    }
    SCON3T &= 0xFE;
    uart5_bytes++;
    return SYS_OK;
}

u8 uart4_send(u8 *buf, u16 len)
{
    if(buf == 0 || len == 0) return SYS_ERR;

    UART4_DIR = 1;
    uart_dir_delay();
    while(len--)
    {
        if(uart4_send_byte(*buf++) != SYS_OK)
        {
            UART4_DIR = 0;
            return SYS_ERR;
        }
    }
    uart_dir_delay();
    UART4_DIR = 0;
    uart4_frames++;
    uart_state &= 0xFFFFFFFEUL;
    return SYS_OK;
}

u8 uart5_send(u8 *buf, u16 len)
{
    if(buf == 0 || len == 0) return SYS_ERR;

    UART5_DIR = 1;
    uart_dir_delay();
    while(len--)
    {
        if(uart5_send_byte(*buf++) != SYS_OK)
        {
            UART5_DIR = 0;
            return SYS_ERR;
        }
    }
    uart_dir_delay();
    UART5_DIR = 0;
    uart5_frames++;
    uart_state &= 0xFFFFFFFDUL;
    return SYS_OK;
}
