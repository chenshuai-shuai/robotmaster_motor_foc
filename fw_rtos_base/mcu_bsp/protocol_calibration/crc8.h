#ifndef _CRC8_H
#define _CRC8_H

#include <stdint.h>

extern uint8_t _lut[256];

void Crc8_init(uint8_t poly); // poly为crc校验常数  0xD5
uint8_t Crc8_calc(uint8_t *data, uint8_t len);

#endif