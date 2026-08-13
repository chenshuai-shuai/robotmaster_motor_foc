/*
 * @Author: skybase
 * @Date: 2025-01-13 17:12:42
 * @LastEditors: skybase
 * @LastEditTime: 2025-02-06 18:44:55
 * @Description:  ᕕ(◠ڼ◠)ᕗ​ 
 * @FilePath: \stm32H7_RT_THREAD\BSP\log\bsp_log.c
 */
#include "bsp_log.h"

char _debug[1024];

DISPLAY_MODE dis_mode = DISPLAY_MODE_DEFAULT; // 显示模式(默认)
FOREGROUND_COLOR fwd_clor = FOREGROUND_COLOR_BLACK; // 字体色（黑色）
BACKGROUND_COLOR bak_clor = BACKGROUND_COLOR_WHITE; // 背景色（白色）


/* USER CODE BEGIN  */
#include "bsp_usart.h"
#include "usart.h"

extern USARTInstance uart10;

/* USER CODE END  */


/**
 * @brief 用户自定义的串口发送
 * 
 * @param data 
 * @param len 
 */
void debug_transmit(uint8_t *data, uint16_t len)
{
    /* 防护：uart10 未初始化（usart_handle=NULL）时静默丢弃，防止 HardFault */
    if (uart10.usart_handle == NULL)
    {
        return;
    }
    USARTSend(&uart10, data, len, USART_TRANSFER_BLOCKING);
}

/* printf 重定向（Retarget）：
 * C 库 printf 默认走半主机（BKPT 断点），脱机运行会 HardFault。
 * 实现 fputc 覆盖弱定义，把字符转发到日志串口。
 */
#include <stdio.h>
int fputc(int ch, FILE *f)
{
    (void)f;
    uint8_t c = (uint8_t)ch;
    debug_transmit(&c, 1);
    return ch;
}


// void Float2Str(char *str, float va)
// {
//     int flag = va < 0;
//     int head = (int)va;
//     int point = (int)((va - head) * 1000);
//     head = abs(head);
//     point = abs(point);
//     if (flag)
//         sprintf(str, "-%d.%d", head, point);
//     else
//         sprintf(str, "%d.%d", head, point);
// }

