#ifndef DDR_H_
#define DDR_H_

#include "main.h"

typedef enum
{
    DDR_8 = 0,
    DDR_16,
} DDRType_enum;

uint8_t DDR8_SumCal(uint8_t *data, int len);
int DDR8_SumCheck(uint8_t *data, int len, uint8_t ddr8);
uint16_t DDR16_SumCal(uint8_t *data, int len);
int DDR16_SumCheck(uint8_t *data, int len, uint16_t ddr16);

#endif