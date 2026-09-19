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
#include "feature_config.h"

#if FEATURE_SERIAL_CTRL
#include "CmdRx_Task.h" /* 串口协议：收到一行命令 → CmdRx_RxIsr() 搬进环形缓冲 */
#endif

USARTInstance uart10;

void UART10_Init(void)
{
    USART_Init_Config_s cfg = {0};
    cfg.usart_handle = &huart6;          /* 默认 USART6，可在 CubeMX 里配置引脚 */
    cfg.recv_buff_size = 128;            /* 命令行接收缓冲（协议行长 ≤128） */
#if FEATURE_SERIAL_CTRL
    cfg.module_callback = CmdRx_RxIsr;   /* DMA+IDLE 收包回调（中断上下文：只搬字节） */
#else
    cfg.module_callback = NULL;
#endif
    USARTRegister(&uart10, &cfg);
}
