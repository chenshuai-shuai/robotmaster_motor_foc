/*
 * disp_st7735s_spi.h - 驱动 B 私有扩展（**不是**契约：页面层不许 include 本文件）
 *
 * 内容：屏尺寸/朝向/偏移标定、行带粒度、CS/RS 宏（双从机反相！）、SPI 实例、字库 ROM 地址常量。
 * 换批次玻璃/换朝向只改这里的宏（页面层与 disp_port.h 不动）。
 */
#ifndef DISP_ST7735S_SPI_H
#define DISP_ST7735S_SPI_H

#include <stdint.h>
#include "main.h"          /* GPIO_TypeDef / HAL 类型 */
#include "board_config.h"  /* BRD_SCR_* 引脚常量（K5：引脚只有这一处出处） */
#include "feature_config.h"

/* ---- 几何 ---- */
#define DISP_ST7735S_W (128u) /* 竖屏宽（列） */
#define DISP_ST7735S_H (160u) /* 竖屏高（行） */

/* 面板自己的 GRAM 尺寸（ST7735S = 132x162，比可见区大）：
 * 点灯期的偏移标定/打底要把**整块 GRAM** 写满，才能保证可见区（不论起点）被覆盖、
 * 且没被我们写到的像素显示黑而不是面板上电默认的白。 */
#define DISP_ST7735S_GRAM_W (132u)
#define DISP_ST7735S_GRAM_H (162u)

/* 坐标偏移标定：不同批次玻璃的可见区偏移不同（常见 0/0、0/24、24/0）。
 * 判据（TM-D1）：上电自检画面的 1px 边框**四边等宽** = 偏移正确；一边宽一边窄就往这边调。 */
#define DISP_ST7735S_XSTART (0u)
#define DISP_ST7735S_YSTART (0u)

/* MADCTL（0x36）朝向：0x00 = 竖屏正向（例程 USE_HORIZONTAL=0）；
 * 另有 0xC0（上下左右都翻）、0x70、0xA0 三种，换朝向只改这里。 */
#define DISP_ST7735S_MADCTL (0x00u)

/* 行带高度（刷新粒度）：128x16x2 = 4096B ≈ 3.1ms @10.5MHz；太小则窗口设置开销占比升高 */
#define DISP_ST7735S_BAND_H (16u)

/* 脏区面积超过屏幕的比例（%）→ 升级为整屏刷新（见 disp_geom.c） */
#define DISP_DIRTY_FULL_RATIO (40u)

/* 开机总线体检：管脚回读 + 面板 RDDID 回读 + 反相闪屏（半周期时长 ms）。
 * 0u = 关（点灯期白屏排查用；2026-09-19 屏已点亮，关掉）。 */
#define DISP_ST7735S_BUSCHECK_MS (0u)

/* ★ 点灯期搜索（速率/偏移标定/电气判据）：0u = 关（2026-09-19 标定完成：速率 /8=10.5MHz 四档全行、
 * 偏移 0/0 无可见误差、CS 每字节脉冲已固化在 lcd_byte）。以后换屏/换批次再置 1u 重跑。 */
#define DISP_ST7735S_DIAG_SEARCH (0u)

/* 开机彩屏体检：整屏红/绿/蓝/白/黑 轮换，每帧停留时长（ms）。
 * 判据（TM-D1）：颜色顺序要对 + 每帧整屏均匀（任一边出现杂色条 = 坐标偏移 → 调 XSTART/YSTART）。
 * 0 = 跳过体检（开机快 0.8s，但屏上没有"面板活着"的直接证据）。 */
#define DISP_ST7735S_COLTEST_MS (200u)

/* ---- 总线 ---- */
#define DISP_SPI (SPI1)                 /* 屏幕口走 SPI1（PA7=MOSI / PB3=SCK / PA6=MISO） */
#define DISP_SPI_BAUD_PRESCALER (SPI_BAUDRATEPRESCALER_8) /* APB2 84MHz / 8 = 10.5MHz（例程上限 15MHz） */

/* ★ CS 是**低=LCD / 高=字库**的双从机反相片选（A 板 OLED 口 pin5）：
 *   · 显示事务全程 CS 保持低；抬高 CS 会把后面的数据写进字库芯片（现象：花屏/不显示）
 *   · 读字库是"进事务→发命令+24 位地址→连读→**出事务**"，进出必须成对（verify_dev 有断言） */
#define DISP_CS_SEL_LCD() HAL_GPIO_WritePin(BRD_SCR_CS_PORT, BRD_SCR_CS_PIN, GPIO_PIN_RESET)
#define DISP_CS_SEL_FONT() HAL_GPIO_WritePin(BRD_SCR_CS_PORT, BRD_SCR_CS_PIN, GPIO_PIN_SET)
#define DISP_RS_CMD() HAL_GPIO_WritePin(BRD_SCR_RS_PORT, BRD_SCR_RS_PIN, GPIO_PIN_RESET)
#define DISP_RS_DATA() HAL_GPIO_WritePin(BRD_SCR_RS_PORT, BRD_SCR_RS_PIN, GPIO_PIN_SET)

/* ---- 板载字库 ROM（GT20L16S1Y 类，GB2312）----
 * 读：CS=高 → 0x03 + A16/A8/A0 → 连读 n 字节 → CS=低
 * ASCII 地址 = (ch - 0x20) * n + BaseAdd；各档 BaseAdd/n 与汉字地址公式见
 * docs/2_方案（定稿设计）/编码方案_彩屏驱动与屏幕抽象层.md §6（出处：模块例程 zk.c）。
 * 点阵格式：横向 8 点/字节、**高位在左**（与内置 OLED_F6x8 的"纵向、LSB 在上"不同，S4 用）。 */
#define DISP_FONT_CMD_READ (0x03u)
#define DISP_FONT_ASCII_BASE_8X16 (0x1DD780u)
#define DISP_FONT_ASCII_N_8X16 (16u)
#define DISP_FONT_ASCII_FIRST_CH (0x20u)
#define DISP_FONT_PROBE_CH ('A')

#endif /* DISP_ST7735S_SPI_H */
