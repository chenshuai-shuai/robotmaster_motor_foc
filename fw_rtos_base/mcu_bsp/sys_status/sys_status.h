/**
 * @file    sys_status.h
 * @brief   系统状态集中管理（LED 指示 + OLED 显示 + 日志三重消费同一状态源）
 *
 * 任何模块迁移状态时调 SYS_SetState()：自动打日志、LED/OLED 任务轮询显示。
 * 卡死定位：日志最后一行 + OLED 屏上状态 = 卡死点。
 */
#ifndef __SYS_STATUS_H
#define __SYS_STATUS_H

#include <stdint.h>

/* ---- 系统状态（扁平枚举，集中定义） ---- */
#define SYS_STATE_BOOT        0U   /* 启动中 */
#define SYS_STATE_INIT        1U   /* 外设初始化 */
#define SYS_STATE_RUNNING     2U   /* 系统运行中 */
#define SYS_STATE_SD_WAIT     3U   /* 等待插卡 */
#define SYS_STATE_SD_INIT     4U   /* SD 卡初始化 */
#define SYS_STATE_SD_MOUNT    5U   /* SD 挂载中 */
#define SYS_STATE_SD_FORMAT   6U   /* SD 格式化中（耗时，勿断电） */
#define SYS_STATE_SD_READY    7U   /* SD 就绪 */
#define SYS_STATE_FAULT       8U   /* 系统异常 */

void SYS_SetState(uint8_t state);      /* 迁移状态（自动打日志） */
uint8_t SYS_GetState(void);            /* 读取当前状态 */
const char *SYS_StateText(uint8_t state);  /* 状态文本（OLED 用） */

#endif /* __SYS_STATUS_H */
