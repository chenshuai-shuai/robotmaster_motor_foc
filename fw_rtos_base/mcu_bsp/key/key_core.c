/*
 * key_core.c - 单键事件机核心逻辑（规则见 key_core.h）
 */
#include "key_core.h"
#include "string.h"

void KeyCore_Init(KeyCore_t *k, uint8_t active_level, uint8_t tick_ms)
{
    memset(k, 0, sizeof(KeyCore_t));
    k->active_level = active_level ? 1u : 0u;
    k->tick_ms = tick_ms ? tick_ms : 10u;
    k->raw_last = 0xFFu; /* 未知：第一个 tick 触发一次采样 */
    k->raw_same = 0u;
    k->stable_pressed = 0u;
}

uint8_t KeyCore_Step(KeyCore_t *k, uint8_t raw_level, KeyCore_Msg_t *msg)
{
    uint8_t out = 0u;
    uint8_t pressed_now = (raw_level == k->active_level) ? 1u : 0u;
    uint32_t debounce_ticks = KEYC_DEBOUNCE_MS / k->tick_ms;

    if (msg != 0)
    {
        msg->evt = KEYC_EVT_NONE;
        msg->hold_ms = 0u;
        msg->gap_ms = 0u;
        msg->clicks = 0u;
    }
    if (debounce_ticks == 0u)
    {
        debounce_ticks = 1u;
    }

    /* ---- 1. 去抖：raw 连续相同达到阈值才认可 ---- */
    if (raw_level != k->raw_last)
    {
        k->raw_last = raw_level;
        k->raw_same = 0u;
    }
    else if (k->raw_same < 0xFFu)
    {
        k->raw_same++;
    }

    /* ---- 2. 时序推进 ---- */
    if (k->stable_pressed)
    {
        k->hold_ms += k->tick_ms;
    }
    else
    {
        k->gap_ms += k->tick_ms;
    }

    /* ---- 3. 空闲且连击窗口超时 → 分派单击/双击 ---- */
    if ((!k->stable_pressed) && (k->click_cnt > 0u) && (k->gap_ms >= KEYC_GAP_MAX_MS))
    {
        if (msg != 0)
        {
            msg->evt = (k->click_cnt >= 2u) ? KEYC_EVT_DOUBLE : KEYC_EVT_CLICK;
            msg->clicks = k->click_cnt;
            msg->hold_ms = k->last_hold;
            msg->gap_ms = k->last_gap;
        }
        if (k->click_cnt >= 2u)
        {
            k->cnt_double++;
        }
        else
        {
            k->cnt_click++;
        }
        k->click_cnt = 0u;
        out = 1u;
    }

    /* ---- 4. 电平稳定后的按下/松手跳变处理 ---- */
    if ((k->raw_same >= debounce_ticks) && (pressed_now != k->stable_pressed))
    {
        if (pressed_now)
        {
            /* 按下：间隔过短视为抖动丢弃 */
            if ((k->gap_ms > 0u) && (k->gap_ms < KEYC_GAP_MIN_MS))
            {
                k->ignore_press = 1u;
                k->cnt_ignored++;
            }
            else
            {
                k->ignore_press = 0u;
                k->last_gap = k->gap_ms;
                if (k->gap_ms > KEYC_GAP_MAX_MS)
                {
                    k->click_cnt = 0u; /* 上一轮已分派，重新计数 */
                }
            }
            k->hold_ms = 0u;
            k->stuck_reported = 0u;
            k->stable_pressed = 1u;
        }
        else
        {
            /* 松手 */
            uint32_t held = k->hold_ms;
            k->stable_pressed = 0u;
            k->gap_ms = 0u;
            k->hold_ms = 0u;

            if (k->stuck_reported)
            {
                /* 本次按下曾被判定"卡死"（电平持续超时）→ 该次按下一律作废：
                   既不产生 CLICK 也不产生 HOLD_RELEASE，防止"按键卡死后松手"
                   误触发使能/失能这类有状态后果的动作。 */
                k->click_cnt = 0u;
            }
            else if (!k->ignore_press)
            {
                if ((held >= KEYC_CLICK_MIN_MS) && (held <= KEYC_CLICK_MAX_MS))
                {
                    k->last_hold = held;
                    k->click_cnt++; /* 短按：计入连击，等窗口超时分派 */
                }
                else if (held > KEYC_CLICK_MAX_MS)
                {
                    k->last_hold = held;
                    if (out == 0u)
                    {
                        if (msg != 0)
                        {
                            msg->evt = KEYC_EVT_HOLD_RELEASE;
                            msg->hold_ms = held;
                            msg->gap_ms = k->last_gap;
                            msg->clicks = k->click_cnt;
                        }
                        k->cnt_hold++;
                        out = 1u;
                    }
                    k->click_cnt = 0u; /* 长按后不与后续短按组成连击 */
                }
                else
                {
                    k->click_cnt = 0u; /* 太短：抖动 */
                }
            }
            k->ignore_press = 0u;
        }
    }

    /* ---- 5. 卡死检测（持续有效电平超时） ---- */
    if (k->stable_pressed && (!k->stuck_reported) && (k->hold_ms >= KEYC_STUCK_MS))
    {
        k->stuck_reported = 1u;
        k->cnt_stuck++;
        if (out == 0u)
        {
            if (msg != 0)
            {
                msg->evt = KEYC_EVT_STUCK;
                msg->hold_ms = k->hold_ms;
            }
            out = 1u;
        }
    }

    return out;
}
