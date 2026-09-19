/*
 * disp_geom.c - 屏幕层纯逻辑实现（宿主机可测，见 disp_geom.h 顶部说明）
 *
 * 所有函数都不碰硬件、不分配内存、不依赖任何工程头 → tools/verify_dev.py 把它拷到临时目录用
 * gcc 直接编译 + 跑断言（DISP 用例）。
 */
#include "disp_geom.h"

void Disp_GeomReset(Disp_GeomRect_t *r)
{
    if (r == 0)
    {
        return;
    }
    r->x0 = 0;
    r->y0 = 0;
    r->x1 = 0;
    r->y1 = 0;
    r->valid = 0u;
}

void Disp_GeomInvalidateAll(Disp_GeomRect_t *r, int16_t fb_w, int16_t fb_h)
{
    if ((r == 0) || (fb_w <= 0) || (fb_h <= 0))
    {
        return;
    }
    r->x0 = 0;
    r->y0 = 0;
    r->x1 = (int16_t)(fb_w - 1);
    r->y1 = (int16_t)(fb_h - 1);
    r->valid = 1u;
}

uint32_t Disp_GeomArea(const Disp_GeomRect_t *r)
{
    if ((r == 0) || (r->valid == 0u))
    {
        return 0u;
    }
    return (uint32_t)(r->x1 - r->x0 + 1) * (uint32_t)(r->y1 - r->y0 + 1);
}

uint8_t Disp_GeomClip(int16_t *x, int16_t *y, int16_t *w, int16_t *h, int16_t fb_w, int16_t fb_h)
{
    int16_t x1;
    int16_t y1;

    if ((x == 0) || (y == 0) || (w == 0) || (h == 0))
    {
        return 0u;
    }
    if ((*w <= 0) || (*h <= 0))
    {
        *w = 0;
        *h = 0;
        return 0u;
    }

    x1 = (int16_t)(*x + *w - 1);
    y1 = (int16_t)(*y + *h - 1);

    if (*x < 0)
    {
        *w = (int16_t)(*w + *x); /* 左边界裁掉 *x 个像素（*x 为负） */
        *x = 0;
    }
    if (*y < 0)
    {
        *h = (int16_t)(*h + *y);
        *y = 0;
    }
    if (x1 > (int16_t)(fb_w - 1))
    {
        x1 = (int16_t)(fb_w - 1);
    }
    if (y1 > (int16_t)(fb_h - 1))
    {
        y1 = (int16_t)(fb_h - 1);
    }
    if ((x1 < *x) || (y1 < *y))
    {
        *w = 0;
        *h = 0;
        return 0u;
    }
    *w = (int16_t)(x1 - *x + 1);
    *h = (int16_t)(y1 - *y + 1);
    return 1u;
}

void Disp_GeomAdd(Disp_GeomRect_t *r, int16_t x, int16_t y, int16_t w, int16_t h,
                  int16_t fb_w, int16_t fb_h, uint32_t full_ratio_pct)
{
    uint32_t area;
    uint32_t screen;

    if ((r == 0) || (fb_w <= 0) || (fb_h <= 0))
    {
        return;
    }
    if (Disp_GeomClip(&x, &y, &w, &h, fb_w, fb_h) == 0u)
    {
        return; /* 完全在屏外：不标脏 */
    }

    if (r->valid == 0u)
    {
        r->x0 = x;
        r->y0 = y;
        r->x1 = (int16_t)(x + w - 1);
        r->y1 = (int16_t)(y + h - 1);
        r->valid = 1u;
    }
    else
    {
        if (x < r->x0)
        {
            r->x0 = x;
        }
        if (y < r->y0)
        {
            r->y0 = y;
        }
        if ((int16_t)(x + w - 1) > r->x1)
        {
            r->x1 = (int16_t)(x + w - 1);
        }
        if ((int16_t)(y + h - 1) > r->y1)
        {
            r->y1 = (int16_t)(y + h - 1);
        }
    }

    /* 面积超阈值 → 升级为整屏（局部刷新反而更亏：带设置/寻址的固定开销摊不开） */
    if ((full_ratio_pct != 0u) && (r->valid != 0u))
    {
        area = Disp_GeomArea(r);
        screen = (uint32_t)fb_w * (uint32_t)fb_h;
        if ((area * 100u) >= (screen * full_ratio_pct))
        {
            Disp_GeomInvalidateAll(r, fb_w, fb_h);
        }
    }
}

uint8_t Disp_GeomGlyphBit(uint8_t col_byte, uint8_t row)
{
    if (row > 7u)
    {
        return 0u;
    }
    return (uint8_t)((col_byte >> row) & 0x01u); /* 纵向 8 点/字节，LSB = 最上面一行 */
}

void Disp_GeomLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                   void (*plot)(int16_t x, int16_t y, void *ctx), void *ctx)
{
    int16_t dx;
    int16_t sx;
    int16_t dy;
    int16_t sy;
    int16_t err;

    if (plot == 0)
    {
        return;
    }

    dx = (int16_t)((x1 > x0) ? (x1 - x0) : (x0 - x1));
    sx = (x0 < x1) ? 1 : -1;
    dy = (int16_t)((y1 > y0) ? (y0 - y1) : (y1 - y0)); /* 负的 |dy|：经典 Bresenham 写法 */
    sy = (y0 < y1) ? 1 : -1;
    err = (int16_t)(dx + dy);

    for (;;)
    {
        plot(x0, y0, ctx);
        if ((x0 == x1) && (y0 == y1))
        {
            break;
        }
        {
            int16_t e2 = (int16_t)(err * 2);

            if (e2 >= dy)
            {
                err = (int16_t)(err + dy);
                x0 = (int16_t)(x0 + sx);
            }
            if (e2 <= dx)
            {
                err = (int16_t)(err + dx);
                y0 = (int16_t)(y0 + sy);
            }
        }
    }
}
