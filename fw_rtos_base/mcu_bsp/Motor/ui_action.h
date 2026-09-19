/*
 * ui_action.h - 板载按键的**本地 UI 策略**（纯逻辑：无 HAL/FreeRTOS 依赖 → 宿主机可穷举测试）
 *
 * ★ 2026-09-17 定稿（docs/协议_串口控制_v1.md §13）：
 *   控制命令全部改走串口协议后，**板载按键不再触碰电机**，只做 UI 导航：
 *     单击 = 下一页 · 双击 = 上一页 · 长按(≥2s) = 回主页(P0) · 卡死 = 该次手势作废
 *   —— 本文件**不存在任何电机动作**（这是可静态验证的安全约定）。
 *
 * 事件机（单击/双击/长按/卡死、去抖、冷却）仍在 mcu_bsp/key/key_core.c，本文件只做"手势 → UI 动作"。
 */
#ifndef UI_ACTION_H
#define UI_ACTION_H

#include "stdint.h"

typedef enum
{
    UI_GES_CLICK = 0, /* 短按 */
    UI_GES_DOUBLE,    /* 双击 */
    UI_GES_HOLD,      /* 长按（松手时判定，hold_ms = 按住时长） */
    UI_GES_STUCK      /* 卡死（该次按下一律作废） */
} Ui_Gesture_e;

typedef enum
{
    UI_ACT_NONE = 0,
    UI_ACT_PAGE_NEXT, /* 下一页（循环） */
    UI_ACT_PAGE_PREV, /* 上一页（循环） */
    UI_ACT_PAGE_HOME  /* 回主页 P0 */
} Ui_Action_t;

/* 长按回主页的最短按住时长（ms） */
#define UI_HOME_HOLD_MS (2000u)

static inline Ui_Action_t Ui_ActionDecide(Ui_Gesture_e ges, uint32_t hold_ms, uint32_t home_ms)
{
    switch (ges)
    {
    case UI_GES_CLICK:
        return UI_ACT_PAGE_NEXT;
    case UI_GES_DOUBLE:
        return UI_ACT_PAGE_PREV;
    case UI_GES_HOLD:
        return (hold_ms >= home_ms) ? UI_ACT_PAGE_HOME : UI_ACT_NONE;
    case UI_GES_STUCK:
    default:
        return UI_ACT_NONE; /* 卡死作废：零副作用 */
    }
}

/* 页码推进（count<=1 时不动） */
static inline uint8_t Ui_PageNext(uint8_t cur, uint8_t count)
{
    if (count <= 1u)
        return 0u;
    return (uint8_t)((cur + 1u) % count);
}

static inline uint8_t Ui_PagePrev(uint8_t cur, uint8_t count)
{
    if (count <= 1u)
        return 0u;
    return (uint8_t)((cur + count - 1u) % count);
}

static inline const char *Ui_ActionStr(Ui_Action_t a)
{
    switch (a)
    {
    case UI_ACT_PAGE_NEXT:
        return "PAGE_NEXT";
    case UI_ACT_PAGE_PREV:
        return "PAGE_PREV";
    case UI_ACT_PAGE_HOME:
        return "PAGE_HOME";
    default:
        return "NONE";
    }
}

#endif /* UI_ACTION_H */
