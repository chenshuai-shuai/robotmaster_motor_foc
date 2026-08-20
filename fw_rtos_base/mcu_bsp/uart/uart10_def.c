/*
 * uart10_def.c - 日志串口实例定义（补全原工程缺失的 uart10）
 *
 * 原工程 bsp_log.c 引用 extern USARTInstance uart10 但未定义，本文件补上。
 * 默认绑定 USART6 —— 若你在 CubeMX 里改用了其他 USART（如 USART3），
 * 把下面的 huart6 换成对应句柄即可。
 *
 * 注意：必须先调用 UART10_Init()（放在 main 初始化之后），日志才能输出。
 */
#include "bsp_usart.h"
#include "usart.h"
#include "main.h"

USARTInstance uart10;

void UART10_Init(void)
{
    USART_Init_Config_s cfg = {0};
    cfg.usart_handle = &huart6;          /* 默认 USART6，可在 CubeMX 里配置引脚 */
    cfg.recv_buff_size = 128;            /* CLI 命令行接收（行长 96） */
    cfg.module_callback = NULL;
    USARTRegister(&uart10, &cfg);
}
