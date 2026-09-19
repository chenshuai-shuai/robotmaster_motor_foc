/*
 * disp_sh1106_i2c.c - 驱动 A：1.3" SH1106（软 I2C，SCL=PB10 / SDA=PB9）→ disp_port 契约
 *
 * S1 策略：**纯包装** —— 一行都不改 mcu_bsp/oled/OLED.c（回归判据 = 屏上内容与改造前逐字一致）。
 *   契约函数 → 现成实现的映射：
 *     Disp_Init      → OLED_Init() + 屏在线探测 + 横幅日志
 *     Disp_Clear     → OLED_Clear()（color!=0 时整屏反色 = 全亮）
 *     Disp_FillRect  → OLED_ClearArea()（黑）/ OLED_DrawRectangle(OLED_FILLED)（亮）
 *     Disp_Text      → OLED_ShowString(..., OLED_6X8 | OLED_8X16)
 *     Disp_Line/Rect → OLED_DrawLine / OLED_DrawRectangle
 *     Disp_Flush     → OLED_Update()（**保持现状**：每帧整屏推，S1 不动性能，免得回归比对掺变量）
 *     Disp_Brightness→ 空实现（has_brightness=0；SH1106 对比度命令留作以后）
 *     Disp_TextCn    → 不支持（has_cn=0）：静默不画
 *   单色 1bpp：color != 0 即点亮 → 页面层的 DISP_C_* 配色在这块屏上只是"亮/灭"，视觉与改造前一致。
 */
/* ★ 本文件被自己的宏整体包住（M3 文件级隔离）。**顺序陷阱**：#if 必须在 include 之后 ——
 *   `#if` 写在 include 之前时该宏尚未定义 → 预处理器按 0 处理 → 整个文件被**静默**编成空对象，
 *   编译期一声不响，直到链接期才报 `L6218E: Undefined symbol Disp_Init`（2026-09-19 实踩）。 */
#include "feature_config.h"

#if FEATURE_DISP_SH1106_I2C

#include "disp_port.h"
#include "disp_geom.h"
#include "OLED.h"
#include "board_config.h"
#include "main.h"
#include "bsp_log.h"

#define DISP_SH1106_W (128u)
#define DISP_SH1106_H (64u)
#define DISP_SH1106_ADDR (0x78u) /* 7 位地址 0x3C 左移一位后的写地址（与 OLED.c 一致） */

/* OLED.c 里的 I2C 引脚原语（**非 static**，可链接）：探测时借用，不修改 OLED.c */
extern void OLED_W_SCL(uint8_t BitValue);
extern void OLED_W_SDA(uint8_t BitValue);

static Disp_Info_t s_info = {
    "SH1106-I2C", DISP_SH1106_W, DISP_SH1106_H, 1u,
    { 6u, 6u, 6u }, { 8u, 8u, 16u }, /* SMALL=6x8, MED/BIG=8x16 */
    0u, /* has_brightness */
    0u  /* has_cn（内置点阵只有少数汉字，S1 不用） */
};

static uint8_t s_inited;
static uint8_t s_ready; /* 单色驱动：初始化在 Disp_Init 里一次做完，这里只是语义对齐（见 Disp_BringUp） */
static uint8_t s_probe_fail;
static uint8_t s_flush_seen;
static volatile uint32_t s_last_flush_ms;
static uint32_t s_last_flush_us;
static uint32_t s_push_cnt;

/* ---- DWT 周期计数（与 OLED.c:117 同一套：不依赖中断/临界区，调度器启动前后都可靠） ---- */
static void disp_dwt_init(void)
{
    if ((DWT->CTRL & DWT_CTRL_CYCCNTENA_Msk) == 0U)
    {
        CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
        DWT->CYCCNT = 0U;
        DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    }
}

static uint32_t disp_cyc_to_us(uint32_t cyc)
{
    uint32_t per_us = SystemCoreClock / 1000000U;

    return (per_us == 0U) ? 0u : (cyc / per_us);
}

/* 屏在线探测（**非破坏性**）：只发地址字节读应答，不发任何命令/数据 → 屏幕内容不变。
 * 为什么要它：把"屏没应答（接线/上拉/供电/地址）"与"屏在线但没刷新（监视任务停）"分开
 * —— OLED.c 内部的 NACK 只打一次日志，驱动层拿不到那个状态。 */
static uint8_t sh1106_probe(void)
{
    uint8_t i;
    uint8_t ack;

    /* ① 总线空闲检查（标准 I2C 起手式）：释放 SDA/SCL 后**必须都读到高**。
     * 为什么非查不可（2026-09-19 实踩）：这两根线在彩屏模块上另有含义（PB9=RS、PB10=CS），
     * 屏选错时模块可能把线拉住 → 第 9 个时钟的"低"会被误当成从机 ACK，自检假报 1。
     * 判据：写 1 释放后仍读到低 = 总线被外部拉死 = 不是"屏在线"。 */
    OLED_W_SDA(1u);
    OLED_W_SCL(1u);
    OLED_W_SDA(1u); /* 每次调用自带 2us 延时：多给一拍让上拉把线拉稳 */
    if (HAL_GPIO_ReadPin(BRD_SCR_I2C_SDA_PORT, BRD_SCR_I2C_SDA_PIN) == GPIO_PIN_RESET)
    {
        LOG_E("disp", "I2C SDA stuck low while idle -> wrong screen? (on the color module PB9=RS, PB10=CS)");
        return 0u;
    }
    if (HAL_GPIO_ReadPin(BRD_SCR_I2C_SCL_PORT, BRD_SCR_I2C_SCL_PIN) == GPIO_PIN_RESET)
    {
        LOG_E("disp", "I2C SCL stuck low while idle -> wrong screen? (check wiring / driver selection)");
        return 0u;
    }

    OLED_W_SDA(0u); /* START：SCL 高电平期间 SDA 下降沿 */
    OLED_W_SCL(0u);

    for (i = 0u; i < 8u; i++)
    {
        OLED_W_SDA((uint8_t)(((DISP_SH1106_ADDR & (0x80u >> i)) != 0u) ? 1u : 0u));
        OLED_W_SCL(1u);
        OLED_W_SCL(0u);
    }

    OLED_W_SDA(1u); /* 释放 SDA，第 9 个时钟读从机应答 */
    OLED_W_SCL(1u);
    ack = (HAL_GPIO_ReadPin(BRD_SCR_I2C_SDA_PORT, BRD_SCR_I2C_SDA_PIN) == GPIO_PIN_RESET) ? 1u : 0u;
    OLED_W_SCL(0u);

    OLED_W_SDA(0u); /* STOP */
    OLED_W_SCL(1u);
    OLED_W_SDA(1u);

    return ack;
}

void Disp_Init(void)
{
    if (s_inited != 0u)
    {
        return;
    }
    disp_dwt_init();

    OLED_Init(); /* 内部：100ms 上电 DWT 忙等 + 引脚开漏 + 初始化序列（不吃 SysTick） */

    s_probe_fail = (sh1106_probe() == 0u) ? 1u : 0u;
    s_inited = 1u;

    /* 屏能力横幅（TM-D1/TM-D5 判据）：name + 分辨率 + 自检码 + 最近一次推屏耗时 */
    s_ready = 1u;
    LOG_I("disp", "%s %ux%u selftest=%u flush=%luus", s_info.name, (unsigned)s_info.w,
          (unsigned)s_info.h, (unsigned)Disp_SelfTest(), (unsigned long)s_last_flush_us);
}

const Disp_Info_t *Disp_Info(void)
{
    return &s_info;
}

void Disp_Clear(uint16_t color)
{
    if (s_ready == 0u)
    {
        return;
    }
    OLED_Clear();
    if (color != DISP_C_BG)
    {
        OLED_Reverse(); /* 单色：非黑 = 全亮 */
    }
}

void Disp_FillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color)
{
    int16_t cx = x;
    int16_t cy = y;
    int16_t cw = w;
    int16_t ch = h;

    if (s_ready == 0u)
    {
        return;
    }
    if (Disp_GeomClip(&cx, &cy, &cw, &ch, (int16_t)s_info.w, (int16_t)s_info.h) == 0u)
    {
        return; /* 完全在屏外 */
    }
    if (color == DISP_C_BG)
    {
        OLED_ClearArea((uint8_t)cx, (uint8_t)cy, (uint8_t)cw, (uint8_t)ch);
    }
    else
    {
        OLED_DrawRectangle((uint8_t)cx, (uint8_t)cy, (uint8_t)cw, (uint8_t)ch, OLED_FILLED);
    }
}

void Disp_Text(int16_t x, int16_t y, const char *s, uint16_t fg, uint16_t bg, Disp_Font_e sz)
{
    (void)fg; /* 单色屏：库内固定"1=亮"画字，前景色值无意义（页面层只要保证不传 DISP_C_BG） */
    (void)bg;

    if ((s_inited == 0u) || (s == NULL) || (x < 0) || (y < 0))
    {
        return;
    }
    OLED_ShowString((uint8_t)x, (uint8_t)y, s,
                    (sz == DISP_FONT_SMALL) ? (uint8_t)OLED_6X8 : (uint8_t)OLED_8X16);
}

void Disp_TextCn(int16_t x, int16_t y, const char *gb2312, uint16_t fg, uint16_t bg, Disp_Font_e sz)
{
    /* has_cn=0：安安静静不画（页面层据 Disp_Info()->has_cn 回落英文标签） */
    (void)x;
    (void)y;
    (void)gb2312;
    (void)fg;
    (void)bg;
    (void)sz;
}

void Disp_Line(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color)
{
    (void)color;

    if ((s_inited == 0u) || (x0 < 0) || (y0 < 0) || (x1 < 0) || (y1 < 0))
    {
        return;
    }
    /* 库接口是 uint8_t：先把端点钳到屏内再画（契约允许的"驱动内钳位"） */
    if (x0 >= (int16_t)s_info.w)
    {
        x0 = (int16_t)(s_info.w - 1u);
    }
    if (x1 >= (int16_t)s_info.w)
    {
        x1 = (int16_t)(s_info.w - 1u);
    }
    if (y0 >= (int16_t)s_info.h)
    {
        y0 = (int16_t)(s_info.h - 1u);
    }
    if (y1 >= (int16_t)s_info.h)
    {
        y1 = (int16_t)(s_info.h - 1u);
    }
    OLED_DrawLine((uint8_t)x0, (uint8_t)y0, (uint8_t)x1, (uint8_t)y1);
}

void Disp_Rect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color, uint8_t filled)
{
    int16_t cx = x;
    int16_t cy = y;
    int16_t cw = w;
    int16_t ch = h;

    (void)color;

    if (s_ready == 0u)
    {
        return;
    }
    if (Disp_GeomClip(&cx, &cy, &cw, &ch, (int16_t)s_info.w, (int16_t)s_info.h) == 0u)
    {
        return;
    }
    OLED_DrawRectangle((uint8_t)cx, (uint8_t)cy, (uint8_t)cw, (uint8_t)ch,
                       (filled != 0u) ? (uint8_t)OLED_FILLED : (uint8_t)OLED_UNFILLED);
}

void Disp_Flush(void)
{
    uint32_t t0;

    if (s_ready == 0u)
    {
        return;
    }
    t0 = DWT->CYCCNT;
    OLED_Update(); /* 整帧 1024B 软 I2C —— S1 保持现状（不改成局部推，免得回归比对掺变量） */
    s_last_flush_us = disp_cyc_to_us((uint32_t)(DWT->CYCCNT - t0));
    s_push_cnt++;

    s_last_flush_ms = HAL_GetTick(); /* 自检心跳（调度器起来后才有意义） */
    s_flush_seen = 1u;
}

void Disp_Brightness(uint8_t pct)
{
    (void)pct; /* has_brightness=0 */
}

uint32_t Disp_LastFlushUs(void)
{
    return s_last_flush_us;
}

/* 推屏次数（本驱动=整帧刷新次数）：flush= 冻住时用它区分"画面没变化"与"推屏停了" */
uint32_t Disp_PushCount(void)
{
    return s_push_cnt;
}

uint8_t Disp_SelfTest(void)
{
    if ((s_inited == 0u) || (s_info.w == 0u))
    {
        return 3u; /* FAIL：没初始化 / 能力无效（0 是 #ST 的"未编译"，驱动不许返回 0） */
    }
    if (s_probe_fail != 0u)
    {
        return 2u; /* WARN：屏无应答（接线/上拉/供电/地址）—— OLED.c 已打过一条 E 日志 */
    }
    if ((s_flush_seen != 0u) && ((uint32_t)(HAL_GetTick() - s_last_flush_ms) >= 2000u))
    {
        return 2u; /* WARN：2s 没推屏 → 屏或监视任务停了 */
    }
    return 1u;
}

/* 单色驱动的"重型初始化"本来就在 Disp_Init 里（软 I2C 快速探测 + 初始化，无长延时），
 * 所以这里实现为空函数：任务调用它没有副作用（契约一致性用）。 */
void Disp_BringUp(void)
{
    /* 空实现：S1 语义不变（屏的初始化已在 Disp_Init 完成） */
}

#endif /* FEATURE_DISP_SH1106_I2C */
