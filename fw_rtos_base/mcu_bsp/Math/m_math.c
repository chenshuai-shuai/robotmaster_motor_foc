/*
 * @Author: skybase
 * @Date: 2024-11-06 23:28:30
 * @LastEditors: skybase
 * @LastEditTime: 2025-03-30 13:47:29
 * @Description:  ᕕ(◠ڼ◠)ᕗ​
 * @FilePath: \mcu_bsp\Math\m_math.c
 */
#include "m_math.h"

/**
 * @brief
 *
 * @param x   映射前的值起始值
 * @param y   映射前的值结束值
 * @param x1  映射后值起始值
 * @param y1  映射后值结束值
 * @param value 需要映射的值
 */
float remap(float x, float y, float x1, float y1, float value)
{
    return x1 + (value - x) * (y1 - x1) / (y - x);
}

/**
 * @brief 快速平方根计算函数
 * 
 * @param number 
 * @return float 
 */
float fast_sqrt(float number)
{
    long i;
    float x, y;
    const float f = 1.5F;
    x = number * 0.5F;
    y = number;
    i = *(long *)&y;
    i = 0x5f3759df - (i >> 1);

    y = *(float *)&i;
    y = y * (f - (x * y * y));
    y = y * (f - (x * y * y));
    return number * y;
}

// >该函数为用户接口
void mathtypeSetVal(mathtype *obj, float init_val, float target_val)
{
    float temp = obj->now_val;
    memset(obj, 0, sizeof(mathtype));
    obj->init_val = init_val;
    obj->target_val = target_val;
    obj->now_val = temp;
}

// >该函数为用户接口
void mathtypeUpdate(mathtype *obj, mathCalUpdate callback, float timebase, float spd)
{
    callback(obj, timebase, spd);
}

/**
 * @brief 一阶线性计算
 * @param timebase 时基,更新时间
 * @param spd 计算的步长,速度,每毫秒(ms)更新的单位步长
 */
void linearCalculation(mathtype *obj, float timebase, float spd)
{

    // 计算增量：每次更新的步长为 spd * timebase
    float increment = spd * timebase;

    // 判断是递增还是递减，并更新 now_val
    if (obj->now_val < obj->target_val)
    {
        obj->now_val += increment;
        if (obj->now_val > obj->target_val)
        {
            obj->now_val = obj->target_val; // 防止超出目标值
        }
    }
    else if (obj->now_val > obj->target_val)
    {
        obj->now_val -= increment;
        if (obj->now_val < obj->target_val)
        {
            obj->now_val = obj->target_val; // 防止低于目标值
        }
    }
    // 如果 now_val 等于 target_val，则不需要更新
}