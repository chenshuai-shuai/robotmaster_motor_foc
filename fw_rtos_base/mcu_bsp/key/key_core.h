/*
 * key_core.h - 单键多功能交互：事件机核心逻辑（纯 C，无 HAL/FreeRTOS 依赖）
 *
 * 为什么单独抽出来：事件判定是"安全相关"逻辑（判错会导致误动作），
 * 抽成纯逻辑后可在宿主机 gcc 上跑时序仿真测试，直接验证判定正确性。
 *
 * 判定规则（时基 = tick_ms，默认 10ms）：
 *   - 消抖：连续相同电平 ≥ KEYC_DEBOUNCE_MS 才认可状态变化
 *   - 短按有效：按下时长 ∈ [40ms, 400ms]（<40ms 按抖动丢弃，>400ms 判长按）
 *   - 连击：松手后 80~300ms 内再次按下 → 连击计数++；<80ms 视为抖动丢弃；
 *           >300ms 视为新的独立单击
 *   - 连击分派：松手后静默满 300ms 才分派（单击动作最多延迟 300ms）
 *   - 长按：松手时按下时长 > 400ms → KEYC_EVT_HOLD_RELEASE（携带 hold_ms，
 *           "多少秒才算生效"由动作层按行配置，如 2000ms）
 *   - 卡死：有效电平持续 ≥ KEYC_STUCK_MS → 报一次 KEYC_EVT_STUCK
 */
#ifndef KEY_CORE_H
#define KEY_CORE_H

#include "stdint.h"

/* ---------------------------- 时间参数（ms，集中调手感） ---------------------------- */
#define KEYC_DEBOUNCE_MS (30u)    /* 消抖窗口 */
#define KEYC_CLICK_MIN_MS (40u)   /* 短按最短有效时长（低于此判抖动） */
#define KEYC_CLICK_MAX_MS (400u)  /* 短按最长时长（高于此判长按） */
#define KEYC_GAP_MIN_MS (80u)     /* 连击最小间隔（低于此判抖动） */
#define KEYC_GAP_MAX_MS (300u)    /* 连击窗口（高于此判独立单击） */
#define KEYC_STUCK_MS (10000u)    /* 卡死判定：持续按下超时 */

/* -------------------------------- 事件定义 -------------------------------- */
typedef enum
{
    KEYC_EVT_NONE = 0,
    KEYC_EVT_CLICK,        /* 单击（连击窗口结束，计数=1） */
    KEYC_EVT_DOUBLE,       /* 双击/多击（计数≥2） */
    KEYC_EVT_HOLD_RELEASE, /* 长按松手（hold_ms = 按住时长，动作层判阈值） */
    KEYC_EVT_STUCK,        /* 卡死（电平持续有效超时） */
} KeyCore_Event_e;

typedef struct
{
    KeyCore_Event_e evt;
    uint32_t hold_ms; /* 本次按下时长（HOLD_RELEASE 时=该次长按时长；CLICK/DOUBLE 时=末次短按时长） */
    uint32_t gap_ms;  /* 相邻两次按下的间隔（手速参考，调试日志用） */
    uint8_t clicks;   /* 连击计数（1=单击，≥2=双击） */
} KeyCore_Msg_t;

typedef struct
{
    /* 配置 */
    uint8_t active_level; /* 有效电平：0=低有效（按下接地），1=高有效 */
    uint8_t tick_ms;      /* 每个 Step 的时基 */
    /* 去抖 */
    uint8_t raw_last;
    uint8_t raw_same;
    uint8_t stable_pressed; /* 已确认的按下状态 */
    /* 时序 */
    uint32_t hold_ms;  /* 当前按住累计 */
    uint32_t gap_ms;   /* 松手后经过时间 */
    uint32_t last_hold;/* 末次按下时长 */
    uint32_t last_gap; /* 末次按下的间隔 */
    uint8_t click_cnt; /* 连击计数（未分派） */
    uint8_t ignore_press;
    uint8_t stuck_reported;
    /* 统计（屏幕显示用） */
    uint32_t cnt_click, cnt_double, cnt_hold, cnt_stuck, cnt_ignored;
} KeyCore_t;

#ifdef __cplusplus
extern "C"
{
#endif

void KeyCore_Init(KeyCore_t *k, uint8_t active_level, uint8_t tick_ms);

/*
 * 每 tick 调用一次；raw_level = GPIO 原始电平（0/1）。
 * 有事件产生时返回 1 并填充 *msg（一次最多一个事件）。
 */
uint8_t KeyCore_Step(KeyCore_t *k, uint8_t raw_level, KeyCore_Msg_t *msg);

#ifdef __cplusplus
}
#endif

#endif /* KEY_CORE_H */
