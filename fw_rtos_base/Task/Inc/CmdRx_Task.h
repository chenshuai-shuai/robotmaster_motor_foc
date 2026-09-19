/*
 * CmdRx_Task.h - 串口命令接收/解析/分派任务（协议 v1 的实现侧）
 *
 * 数据路径：USART6 DMA+IDLE 收包 → bsp_usart 的 module_callback（**中断上下文**）
 *          → CmdRx_RxIsr() 只做"字节搬进环形缓冲"（无日志/无浮点/无 CAN）
 *          → 本任务（prio 4）拼行 → Cmd_ParseLine() → 分派 → @OK/@ERR 回包（Proto_Send）
 *
 * 线程约定：本任务只**改控制状态 + 发请求**，绝不直接发 CAN（CAN 由 J8108 任务独占）。
 */
#ifndef CMDRX_TASK_H
#define CMDRX_TASK_H

#include "stdint.h"

void CmdRx_Task_Init(void);
void CmdRx_RxIsr(void); /* bsp_usart module_callback（中断上下文；只搬字节） */

/* 供 UI/遥测显示 */
uint32_t CmdRx_RxLines(void);
uint32_t CmdRx_RxBytes(void); /* 原始接收字节数（诊断：区分"没字节"与"有字节没换行"） */
const char *CmdRx_LastCmd(void);
uint8_t CmdRx_LastErr(void); /* 最近一次 @ERR 码（0=无） */

#endif /* CMDRX_TASK_H */
