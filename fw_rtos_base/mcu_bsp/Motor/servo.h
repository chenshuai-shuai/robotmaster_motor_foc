/*
 * @Author: skybase
 * @Date: 2024-11-07 22:48:13
 * @LastEditors: skybase
 * @LastEditTime: 2024-12-19 11:07:40
 * @Description:  ᕕ(◠ڼ◠)ᕗ​
 * @FilePath: \MDK-ARMd:\Project\Embedded_project\Stm_pro\luntui\BSP\Motor\servo.h
 */
#ifndef SERVO_H
#define SERVO_H

#include "m_math.h"
#include "tim.h"

typedef struct servo_s
{
    float deg;
    int degmode;  //  mode为0:参数为弧度制, mode为1:参数为角度制
    int degrang;  // 表明舵机为90°/180°/270°
    int polarity; // 舵机的极性,用于矫正旋转正反,0:正常 1:反转

    TIM_HandleTypeDef *htim;
    uint32_t TIM_CHANNEL;

    float remap_deg_s;
    float remap_deg_e;

} servo_s;

void servoInit(servo_s *servo, TIM_HandleTypeDef *htim, uint32_t TIM_CHANNEL, float remap_deg_s, float remap_deg_e, int degrang, int degmode, int polority);
uint32_t servoSetDeg(servo_s *servo, float deg);
void servoToDeg(servo_s *servo, float deg);

#endif