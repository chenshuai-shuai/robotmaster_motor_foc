/*
 * Monitor_Task.h - 低频监控任务（屏幕刷新 + 日志摘要 + 协议遥测）
 *
 * 2026-09-17 定稿（docs/协议_串口控制_v1.md §10.1）：
 *   **一个任务，三个出口，同源快照**——
 *     屏刷 10Hz（100ms）/ 日志 1Hz（1000ms）/ 协议遥测 @TEL（默认 20Hz，可 #TEL 调）
 *   优先级 3（低）：软 I2C 刷屏 ms 级阻塞，**绝不能**挤进 prio 5 的 200Hz 控制帧发生器。
 *   数据只读 J8108 的"帧一致快照"（seqlock），且只在**数据新鲜**时输出数值。
 */
#ifndef MONITOR_TASK_H
#define MONITOR_TASK_H

#include "stdint.h"
#include "ui_status.h"

void Monitor_Task_Init(void);

/* 遥测（#TEL） */
void Monitor_SetTelPeriod(uint16_t ms); /* 0=关（最小 2ms） */
uint16_t Monitor_GetTelPeriod(void);
void Monitor_FormatTel(char *buf, uint32_t bufsz); /* 生成一行 "@TEL ..."（周期发送与 #TEL 1 共用） */

/* 当前 UI 状态视图（只读，供调试） */
const Ui_Status_t *Monitor_Ui(void);

/* 模块自检（非破坏性，只读）：0=未编译 1=OK（任务在跑、循环新鲜） 2=WARN 3=FAIL */
uint8_t Monitor_SelfTest(void);

#endif /* MONITOR_TASK_H */
