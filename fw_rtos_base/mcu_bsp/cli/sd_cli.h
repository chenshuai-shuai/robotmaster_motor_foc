/**
 * @file    sd_cli.h
 * @brief   SD 卡命令行（STM32 版 Linux 终端）：ls/mkdir/cat/write/rm 等交互操作
 *
 * 通过日志串口（USART6，uart10 实例）接收命令、返回结果。
 * 支持命令：help ls mkdir rm mv cat write append stat df
 * FatFs 单任务独占（CLI 任务），SdCard 挂载完成后 FS 归 CLI 使用。
 */
#ifndef __SD_CLI_H
#define __SD_CLI_H

#include <stdint.h>

void SD_CLI_Init(void);            /* 注册 uart10 接收回调 + 打印欢迎横幅 */
void SD_CLI_TaskEntry(void *arg);  /* CLI 任务入口（等接收通知 → 解析执行） */

#endif /* __SD_CLI_H */
