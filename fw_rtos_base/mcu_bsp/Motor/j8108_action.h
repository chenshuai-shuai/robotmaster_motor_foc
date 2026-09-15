/*
 * j8108_action.h - 8108 单按键"动作策略层"（**纯逻辑，无 HAL/FreeRTOS 依赖 → 可宿主机测试**）
 *
 * 设计依据：docs/设计_8108监护界面与单键交互.md §3（方案 v2）
 *   手势：只有"长按"能触发有状态后果的动作；连击类（单击/双击）只做零副作用 UI（光标移动）。
 *   触发：**松手时**判定，按住时长 > 2s（用户 2026-09-15 定案）→ 通过 CAN 发送数据。
 *   行绑定：只有 J8108_ACTION_ROW（行0 = CAN）有动作；其余行是只读行 → 长按**静默无动作**。
 *
 * 保护层（L2/L3 在本文件；L1 手势在 key_core；L3.5/L4 在 J8108_Task.c 的轮询里）：
 *   L1 手势把守：2s + 松手生效（+ 中途松手取消由时长判定天然实现）
 *   L2 冷却锁定：两次动作间隔 < 1.5s → 拒绝（防连按抖动导致使能/失能来回切）
 *   L3 前置校验：CAN 未初始化/总线故障 → 拒绝执行（不往坏总线上打帧）
 *   L3.5 发后 ACK 检查：20ms 内帧未被任何节点应答 → 主动丢弃 + 报"无节点"
 *        （本板 AutoRetransmission=ENABLE，不丢弃会无限重传 → 触发总线错误）
 *   L4 执行后确认：300ms 内反馈应出现（使能）/ 停止（失能），否则报 `?`
 */
#ifndef J8108_ACTION_H
#define J8108_ACTION_H

#include "stdint.h"

/* ---- 动作行与时间参数 ---- */
#define J8108_ACTION_ROW (0u)            /* 唯一绑定动作的行：行0 = CAN（使能/失能） */
#define J8108_ACTION_HOLD_MS (2000u)     /* 最短按住时长：>2s 即触发 */
#define J8108_ACTION_COOLDOWN_MS (1500u) /* L2 冷却 */
#define J8108_ACTION_ACK_MS (20u)        /* L3.5 发后多久检查是否被 ACK */
#define J8108_ACTION_CONFIRM_MS (300u)   /* L4 发后多久检查反馈是否按预期变化 */

/* ---- CAN 状态（与 motor_8108.h 的 J8108_Status_e 数值一致；此处不复用其枚举，保持本文件零依赖）---- */
#define ACT_ST_INIT_FAIL (0u)
#define ACT_ST_INIT_OK (1u)
#define ACT_ST_READY (2u)
#define ACT_ST_BUS_ERR (3u)

typedef enum
{
    ACT_DO_NOTHING = 0,   /* 只读行长按：静默（无动作、无提示） */
    ACT_ENABLE,           /* 发使能帧 */
    ACT_DISABLE,          /* 发失能帧 */
    ACT_REJECT_SHORT,     /* 按住不够长 */
    ACT_REJECT_COOLDOWN,  /* L2 */
    ACT_REJECT_CAN        /* L3 */
} J8108_Action_t;

/* 决策：纯函数（同样的输入永远同样输出，便于宿主机穷举测试） */
static inline J8108_Action_t J8108_ActionDecide(uint8_t cursor_row, uint32_t hold_ms,
                                                uint8_t cooldown_ok, uint8_t can_status)
{
    if (cursor_row != J8108_ACTION_ROW)
    {
        return ACT_DO_NOTHING; /* 只读行：静默 */
    }
    if (hold_ms < J8108_ACTION_HOLD_MS)
    {
        return ACT_REJECT_SHORT;
    }
    if (cooldown_ok == 0u)
    {
        return ACT_REJECT_COOLDOWN;
    }
    if ((can_status == ACT_ST_INIT_FAIL) || (can_status == ACT_ST_BUS_ERR))
    {
        return ACT_REJECT_CAN;
    }
    /* 反馈帧在流 = 电机已使能（厂商文档：使能后才回传反馈帧）→ 长按=失能；否则长按=使能 */
    return (can_status == ACT_ST_READY) ? ACT_DISABLE : ACT_ENABLE;
}

/* ---- 最近一次动作结果（屏上行7 显示 + 日志；ASCII）---- */
typedef enum
{
    ACTR_NONE = 0,
    ACTR_EN_SENT,        /* 使能帧已发出，等确认 */
    ACTR_DIS_SENT,       /* 失能帧已发出，等确认 */
    ACTR_EN_OK,          /* 使能确认：反馈帧出现 */
    ACTR_EN_NO_FB,       /* 使能后 300ms 无反馈 → 电机/接线/CANID 待查 */
    ACTR_EN_NO_ACK,      /* 发出去的帧没被任何节点 ACK → 总线上没有节点 */
    ACTR_DIS_OK,         /* 失能确认：反馈帧停止 */
    ACTR_DIS_STILL_FB,   /* 失能后反馈仍在 → 帧可能被丢弃 */
    ACTR_SHORT,          /* 按住时长不足 */
    ACTR_COOLDOWN,       /* L2 冷却中 */
    ACTR_CAN_NOT_READY   /* L3 CAN 不可用 */
} J8108_ActionResult_e;

static inline const char *J8108_ActionResultStr(J8108_ActionResult_e r)
{
    switch (r)
    {
    case ACTR_EN_SENT:
        return "EN SENT...";
    case ACTR_DIS_SENT:
        return "DIS SENT...";
    case ACTR_EN_OK:
        return "EN OK (FB UP)";
    case ACTR_EN_NO_FB:
        return "EN NO FEEDBACK";
    case ACTR_EN_NO_ACK:
        return "EN NO ACK/NODE";
    case ACTR_DIS_OK:
        return "DIS OK";
    case ACTR_DIS_STILL_FB:
        return "DIS STILL FB";
    case ACTR_SHORT:
        return "HOLD <2.0s";
    case ACTR_COOLDOWN:
        return "COOLDOWN";
    case ACTR_CAN_NOT_READY:
        return "CAN NOT READY";
    default:
        return "";
    }
}

#endif /* J8108_ACTION_H */
