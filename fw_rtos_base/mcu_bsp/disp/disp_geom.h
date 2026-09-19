/*
 * disp_geom.h - 屏幕层的**纯逻辑**（无 HAL / 无 FreeRTOS 依赖 → 可在 PC 上跑单元测试）
 *
 * 为什么单独拆一文件：脏矩形合并 / 直线光栅化 / 位模取位 是最容易写错、又最难在板上调试的逻辑；
 * 放这里就能用宿主机 gcc 穷举验证（范式同 mcu_bsp/ctrl/ctrl_core.c、mcu_bsp/key/key_core.c），
 * 进 `make verify` 的宿主机测试（tools/verify_dev.py 的 DISP 用例）。
 *
 * 依赖规则：本文件只许 include <stdint.h>（不许 include HAL/FreeRTOS/其它驱动头）。
 */
#ifndef DISP_GEOM_H
#define DISP_GEOM_H

#include <stdint.h>

/* 脏矩形（闭区间边界，含 x1/y1）；valid=0 表示"没有脏区" */
typedef struct
{
    int16_t x0, y0, x1, y1;
    uint8_t valid;
} Disp_GeomRect_t;

/* 清空脏区（上屏完成后调用） */
void Disp_GeomReset(Disp_GeomRect_t *r);

/* 把矩形并入脏区（先按屏幕尺寸裁剪；并集面积 ≥ full_ratio_pct% 屏幕 → 升级为整屏）。
 * full_ratio_pct 传 0 表示"永远不升级"（不推荐）。 */
void Disp_GeomAdd(Disp_GeomRect_t *r, int16_t x, int16_t y, int16_t w, int16_t h,
                  int16_t fb_w, int16_t fb_h, uint32_t full_ratio_pct);

/* 直接标脏整屏 */
void Disp_GeomInvalidateAll(Disp_GeomRect_t *r, int16_t fb_w, int16_t fb_h);

/* 脏区面积（像素数；valid=0 → 0） */
uint32_t Disp_GeomArea(const Disp_GeomRect_t *r);

/* 越界裁剪：原地修正 x/y/w/h；完全在屏外 → 返回 0（调用者应直接 return） */
uint8_t Disp_GeomClip(int16_t *x, int16_t *y, int16_t *w, int16_t *h, int16_t fb_w, int16_t fb_h);

/* 位模取位：内置点阵为"纵向 8 点/字节、LSB 在上"→ 返回 col_byte 在 row(0..7) 行的像素 */
uint8_t Disp_GeomGlyphBit(uint8_t col_byte, uint8_t row);

/* 直线光栅化（Bresenham，两端点都画）：每点回调画点函数（回调负责写显存/裁剪） */
void Disp_GeomLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                   void (*plot)(int16_t x, int16_t y, void *ctx), void *ctx);

#endif /* DISP_GEOM_H */
