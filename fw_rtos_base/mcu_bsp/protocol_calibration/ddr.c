/*
 * @Author: skybase
 * @Date: 2025-01-12 15:15:27
 * @LastEditors: skybase
 * @LastEditTime: 2025-01-12 15:45:04
 * @Description:  用于方便计算和检验DDR校验和的库
 * @FilePath: \luntui\BSP\ddr\ddr.c
 */
#include "ddr.h"

/**
 * @brief 用于计算数据帧的DDR8校验和
 *
 * @param len 只包含数据位，不包含校验位
 */
uint8_t DDR8_SumCal(uint8_t *data, int len)
{
    uint16_t ddr_sum = 0;
    for (int i = 0; i < len; i++)
        ddr_sum += *(data + i);
    return (uint8_t)ddr_sum;
}

int DDR8_SumCheck(uint8_t *data, int len, uint8_t ddr8)
{
    uint8_t ddr_sum = DDR8_SumCal(data, len);

    if (ddr_sum == ddr8)
        return 0;
    return 1;
}

/**
 * @brief 用于计算数据帧的DDR16校验和
 *
 * @param len 只包含数据位，不包含校验位
 */
uint16_t DDR16_SumCal(uint8_t *data, int len)
{
    uint16_t ddr_sum = 0;
    for (int i = 0; i < len; i++)
        ddr_sum += *(data + i);
    return ddr_sum;
}

int DDR16_SumCheck(uint8_t *data, int len, uint16_t ddr16)
{
    uint8_t ddr_sum = DDR16_SumCal(data, len);

    if (ddr_sum == ddr16)
        return 0;
    return 1;
}