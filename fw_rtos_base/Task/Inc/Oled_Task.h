/*
 * Oled_Task.h - OLED 页面渲染器（**不含任务**：任务壳在 Monitor_Task.c）
 *
 * 2026-09-17 定稿：屏幕专职显示 4 页（LINK / MOTION / SERIAL / SYSTEM），
 *   按键只做翻页（UI），数据只来自"帧一致快照"，**局部刷新**（只重画变化的行）。
 */
#ifndef OLED_TASK_H
#define OLED_TASK_H

#include "stdint.h"
#include "motor_8108.h"
#include "ui_status.h"

/* 按页绘制（page 越界 → 画 P0）；内部做局部刷新，无变化不发 I2C */
void Oled_UiDraw(uint8_t page, const J8108_Snapshot_t *sn, const Ui_Status_t *ui);

/* 模块自检（非破坏性，只读）：0=未编译 1=OK 2=WARN（最近没在刷） 3=FAIL */
uint8_t Oled_SelfTest(void);

#endif /* OLED_TASK_H */
