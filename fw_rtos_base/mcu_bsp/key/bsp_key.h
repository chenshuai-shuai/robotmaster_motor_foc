/*
 * bsp_key.h - 板载用户按键（A 板 KEY = PB2）驱动 + 事件分发
 *
 * 硬件：原理图 sheet9「OLED&按键」：KEY(PB2)；本模块**自带 GPIO 初始化**，
 *       不修改 CubeMX 生成区（gpio.c / .ioc / main.c 一行不动）。
 * 注意：PB2 是 STM32F4 的 BOOT1 复用脚；本板 BOOT0=0（Flash 启动），
 *       故 BOOT1 仅在 BOOT0=1 时决定启动介质，此处当普通输入使用安全。
 * 分发：事件 → FreeRTOS 事件标志组（各消费者各等各自的位，互不干扰）
 */
#ifndef BSP_KEY_H
#define BSP_KEY_H

#include "stdint.h"
#include "FreeRTOS.h"
#include "event_groups.h"
#include "key_core.h"

/* ------------------------------ 硬件定义 ------------------------------ */
#define KEY_GPIO_PORT (GPIOB)
#define KEY_GPIO_PIN (GPIO_PIN_2)
#define KEY_GPIO_PULL (GPIO_PULLUP) /* 内部上拉兜底（若板上已有外部上拉也无害） */
#define KEY_ACTIVE_LEVEL (1u)       /* 有效电平：1=按下为高。**A 板实测 2026-09-15**：PB2 空闲为低、按下为高（高有效）；换板/改电路看开机日志 idle raw level 再定 */

/* ------------------------------ 时间参数 ------------------------------ */
#define KEY_SCAN_TICK_MS (10u)      /* 扫描周期（= key_core 时基） */
#define KEY_TASK_PRIORITY (4u)      /* 低于控制任务(5)，高于显示任务(3) */
#define KEY_TASK_STACK_WORDS (256u)

/* ------------------------------ 事件位 ------------------------------ */
#define KEY_BIT_CLICK (1u << 0)        /* 单击（UI：光标下移） */
#define KEY_BIT_DOUBLE (1u << 1)       /* 双击（UI：光标上移） */
#define KEY_BIT_HOLD_RELEASE (1u << 2) /* 长按松手（动作层判阈值，如 2s） */
#define KEY_BIT_STUCK (1u << 3)        /* 卡死检测（忽略该键 + 告警） */

#ifdef __cplusplus
extern "C"
{
#endif

void Key_Init(void);      /* GPIO + 内核初始化（幂等） */
void Key_Task_Init(void); /* 创建事件组 + 按键扫描任务（调度器启动前调用亦可） */

EventGroupHandle_t Key_EventGroup(void);
uint8_t Key_IsPressed(void);   /* 当前是否按住（已消抖） */
uint32_t Key_GetHoldMs(void);  /* 当前按住时长 ms（长按进度条用） */
uint8_t Key_GetRawLevel(void); /* GPIO 原始电平（实测方向用） */
void Key_GetCore(KeyCore_t *out);                  /* 统计数据拷贝（屏幕显示用） */
void Key_GetLastMsg(KeyCore_Msg_t *out, uint32_t *ms); /* 最近一次事件快照（提示行用，不消费事件位） */

#ifdef __cplusplus
}
#endif

#endif /* BSP_KEY_H */
