/*
 * disp_port.h - 屏幕抽象层契约（唯一对外接口，UI 层只认这里的 Disp_*）
 *
 * 目的：把"页面逻辑"和"具体屏"解耦 —— 换屏 = 改 feature_config.h §4 的宏 + 换驱动 .c，
 *       页面层(Task/Src/Oled_Task.c)一行不动。依据：docs/2_方案（定稿设计）/编码方案_彩屏驱动与屏幕抽象层.md
 *
 * 两条硬机制（tools/verify_dev.py 断言，不靠自觉）：
 *   1. UI 层白名单：Oled_Task.c 里不许出现 OLED_ / HAL_GPIO / SPI1 / hspi 等驱动私有符号；
 *   2. 未选中的驱动实现文件**整文件被自己的宏包住** → 编译成空对象（两个驱动都实现 Disp_Init
 *      会重复定义，Keil 会编译所有在工程里的文件，所以必须靠宏二选一）。
 *
 * 语义（有争议以本节为准）：
 *   坐标   : 原点左上；x∈[0,w)、y∈[0,h)；**越界由驱动裁剪**，不崩
 *   颜色   : 彩屏 = RGB565；单色屏 = color != 0 即点亮。页面层只用 DISP_C_*，不写裸值
 *   刷新   : 绘制只改显存/脏区；**只有 Disp_Flush() 上屏**（驱动自选策略：单色=页推，彩屏=脏矩形行带）
 *   时间预算: 单次绘制调用 < 5ms（Monitor 是 prio3，不许堵住控制/协议任务）；
 *            **禁止 vTaskDelay、禁止无界自旋**；重活（整屏清）只允许出现在 Disp_Init
 *   调用者 : Disp_Init 在 main()（调度器启动前）；绘制只在 Monitor 任务里调（单线程，无并发）；
 *            Disp_SelfTest 只读状态，可从别的任务（#ST 命令）调
 *   失败降级: Disp_Info()->w == 0 → 页面层所有绘制变空操作（屏坏了系统照跑）
 *
 * 新增驱动的检查表：实现本文件全部 Disp_*（**不许返回 0**，0 是 #ST 的"本固件没编译该模块"）
 *   → 把 .c 从首行到末行用 #if FEATURE_DISP_xxx ... 包住（末行写带宏名的 #endif 注释标记）
 *   → 加进 uvprojx（BSP 组）+ IncludePath 加 ../mcu_bsp/disp
 *   → `python tools/check_profiles.py --fast`（含 DISP_ST7735S 驱动变体）过一遍。
 */
#ifndef DISP_PORT_H
#define DISP_PORT_H

#include <stdint.h>
#include "feature_config.h"

/* 恰好选中一个驱动：feature_config.h §6 已经拦了一层，这里再拦一层 ——
 * 本头文件被**单独 include**（V11 契约头检查）时也要给出明确报错，而不是默默编出空对象。 */
#if ((FEATURE_DISP_SH1106_I2C == 0u) && (FEATURE_DISP_ST7735S_SPI == 0u))
#error "disp_port.h: no display driver selected (see feature_config.h section 4)"
#endif
#if ((FEATURE_DISP_SH1106_I2C != 0u) && (FEATURE_DISP_ST7735S_SPI != 0u))
#error "disp_port.h: two display drivers selected (they share the same pins)"
#endif

/* 字号档位（页面层只认这三档；驱动负责映射到实际点阵） */
typedef enum
{
    DISP_FONT_SMALL = 0u, /* 行内文本（小） */
    DISP_FONT_MED = 1u,   /* 强调（中） */
    DISP_FONT_BIG = 2u    /* 大号数值（大） */
} Disp_Font_e;

/* 屏能力描述（Disp_Info() 返回；页面层据此排版，不写死 128x64） */
typedef struct
{
    const char *name;      /* "SH1106-I2C" / "ST7735S-SPI"：开机横幅用 */
    uint16_t w, h;         /* 逻辑分辨率（当前朝向），竖屏 = 128x160 */
    uint8_t color_bits;    /* 1=单色，16=RGB565 */
    uint8_t font_w[3];     /* SMALL/MED/BIG 的字符格宽（页面层排版用） */
    uint8_t font_h[3];     /* SMALL/MED/BIG 的字符格高（= 行高） */
    uint8_t has_brightness;/* 1=支持 Disp_Brightness */
    uint8_t has_cn;        /* 1=支持 Disp_TextCn（汉字）；0 时页面层自动回落英文标签 */
} Disp_Info_t;

/* 主题色（RGB565；单色驱动把"非 0"视为点亮 → 页面层用同一份配色代码两种屏都能看） */
#define DISP_C_BG 0x0000u   /* 背景：黑 */
#define DISP_C_FG 0xFFFFu   /* 正文：白 */
#define DISP_C_OK 0x07E0u   /* 正常/已建立：绿 */
#define DISP_C_WARN 0xFFE0u /* 预警/限幅：黄 */
#define DISP_C_ERR 0xF800u  /* 错误/断链/急停：红 */
#define DISP_C_DIM 0x8410u  /* 单位/提示：灰 */

/* 初始化：幂等；自带 GPIO/AF/SPI 初始化；调度器启动前可调用（内部只用 DWT 忙等，不用 HAL_Delay）。
 * 内部会画"自检画面"并在日志打一条 `disp: <name> <w>x<h> selftest=<n> flush=<us>us`（TM-D1 判据）。 */
void Disp_Init(void);

/* 屏的"重型初始化 + 自检"（上电稳定等待、软复位/唤醒、字库探测、自检/彩色体检、
 * 点灯期的速率扫描与静态窗口）。**由显示任务在调度器启动后调用**，不在 main 里做：
 *   这些步骤合计 0.4~4s，留在 main/Disp_Init 会把"调度器启动"整个卡住——期间日志任务与
 *   流水灯任务都还没被调度，现象=开机几秒全静默然后突然一起开始（本项目 2026-09-19 实踩）。
 * 调用完成后绘制才生效（驱动内部按 ready 门控）；重复调用无副作用。
 * 单色驱动的这一步骤本来就在它的 Disp_Init 里（快），实现为空函数。 */
void Disp_BringUp(void);

/* 能力查询：永不为 NULL；初始化失败时 w/h == 0 → 页面层自动降级为空操作 */
const Disp_Info_t *Disp_Info(void);

void Disp_Clear(uint16_t color);
void Disp_FillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
void Disp_Text(int16_t x, int16_t y, const char *s, uint16_t fg, uint16_t bg, Disp_Font_e sz);
/* 汉字：GB2312 编码（"位置" = "\xCE\xBB\xD6\xC3"）；不支持时静默不画（见 Disp_Info()->has_cn） */
void Disp_TextCn(int16_t x, int16_t y, const char *gb2312, uint16_t fg, uint16_t bg, Disp_Font_e sz);
void Disp_Line(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color);
void Disp_Rect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color, uint8_t filled);

/* 唯一"上屏"动作；同时是自检心跳（2s 没被调用 → #ST disp 报 WARN：屏/监视任务停了） */
void Disp_Flush(void);

void Disp_Brightness(uint8_t pct); /* 不支持则空实现（Disp_Info()->has_brightness==0） */

/* 自检（非破坏性只读）：1=OK 2=WARN（屏无应答 / 字库芯片没读到 / 2s 没刷新）3=FAIL（没初始化/SPI 挂了）
 * ⚠ 驱动**不许返回 0**：0 保留给 #ST 的"本固件没编译该模块"（协议口径见 docs/操作手册_串口命令.md）。 */
uint8_t Disp_SelfTest(void);

/* 最近一次真正推屏的耗时（us）；给日志与台架判据用（TM-D2）。
 * 单色驱动也实现：软 I2C 整帧耗时是它的关键性能指标。 */
uint32_t Disp_LastFlushUs(void);

/* 累计推屏次数：`flush=` 数值冻住时用它区分"画面没变化（正常）"与"推屏停了（故障）"。
 * 两个驱动都必须实现（页面层/台架会打印它）。 */
uint32_t Disp_PushCount(void);

#endif /* DISP_PORT_H */
