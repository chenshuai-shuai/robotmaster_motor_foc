/*
 * @Author: skybase
 * @Date: 2025-03-30 13:38:54
 * @LastEditors: skybase
 * @LastEditTime: 2025-03-30 13:51:08
 * @Description:  ᕕ(◠ڼ◠)ᕗ​
 * @FilePath: \mcu_bsp\protocol_calibration\crc8.c
 */
#include "crc8.h"

uint8_t _lut[256];

/**
 * @brief CRC8校验初始化
 *
 * @param poly CRC8多项式常数
 * @note 该函数在程序初始化时调用一次即可
 */
void Crc8_init(uint8_t poly)
{
    for (int idx = 0; idx < 256; ++idx)
    {
        uint8_t crc = idx;
        for (int shift = 0; shift < 8; ++shift)
        {
            crc = (crc << 1) ^ ((crc & 0x80) ? poly : 0);
        }
        _lut[idx] = crc & 0xff;
    }
}

/**
 * @brief  CRC8校验计算
 *
 * @param data 数据指针
 * @param len  数据长度
 * @return uint8_t
 */
uint8_t Crc8_calc(uint8_t *data, uint8_t len)
{
    uint8_t crc = 0;
    while (len--)
    {
        crc = _lut[crc ^ *data++];
    }
    return crc;
}
