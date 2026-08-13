/*
 * @Author: skybase
 * @Date: 2025-01-17 04:15:13
 * @LastEditors: skybase
 * @LastEditTime: 2025-01-17 04:32:13
 * @Description:  ᕕ(◠ڼ◠)ᕗ​ 
 * @FilePath: \BSP\filter\data_filter.h
 */

#ifndef DATA_FILTER_H_
#define DATA_FILTER_H_

#include "main.h"

#define Pi 3.1415926f
#define HP_CUT_FRQ 5
#define SAMPLE_RATE  200.0f
#define WIN_NUM 5

void high_pass_filter(float in, float *out);
void LowPassFilter_RC(float Vi, float *Vo);
float window_Filter(float data, float *buf, uint8_t len);

#endif /* DATA_FILTER_H_ */
