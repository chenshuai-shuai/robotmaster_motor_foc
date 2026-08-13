#include "servo.h"

/**
 * @brief
 * @param servo
 * @param htim
 * @param TIM_CHANNEL
 * @param remap_deg_s 0°对应的duty值
 * @param remap_deg_e 90°/180°/270°对应的duty值
 */
void servoInit(servo_s *servo, TIM_HandleTypeDef *htim, uint32_t TIM_CHANNEL, float remap_deg_s, float remap_deg_e, int degrang, int degmode, int polarity)
{
    memset(servo, 0, sizeof(servo_s));
    servo->htim = htim;
    servo->TIM_CHANNEL = TIM_CHANNEL;
    servo->remap_deg_s = remap_deg_s;
    servo->remap_deg_e = remap_deg_e;
    servo->degmode = degmode;
    servo->degrang = degrang;
    servo->polarity =
        polarity;

    HAL_TIM_PWM_Start(htim, TIM_CHANNEL);
}

/**
 * @description:
 * @param {servo_s} *servo
 * @param {float} deg
 */
uint32_t servoSetDeg(servo_s *servo, float deg)
{
    if (deg < 0)
    {
        deg = 0;
        servo->deg = 0;
    }

    if (servo->degmode == 1)
    {
        if (deg > servo->degrang)
        {
            deg = servo->degrang;
        }
        servo->deg = deg;

        deg = servo->polarity == 0 ? deg : servo->degrang - deg;
        deg = (int)(deg * 100) % 36000;
    }
    else
    {
        if (deg > servo->degrang * 6.28 / 360)
        {
            deg = servo->degrang * 6.28 / 360;
        }
        servo->deg = deg;

        deg = servo->polarity == 0 ? deg : servo->degrang * 6.28 / 360 - deg;
        deg = (int)(deg * 1000) % 6280;
    }

    return deg;
}

/**
 * @description:
 * @param {servo_s} *servo
 * @param {float} deg
 */
void servoToDeg(servo_s *servo, float deg)
{
    float duty = 0;
    uint32_t deg_t = servoSetDeg(servo, deg);
    if (servo->degmode == 1)
        duty = remap(0, servo->degrang * 100, servo->remap_deg_s, servo->remap_deg_e, deg_t);
    else
        duty = remap(0, servo->degrang * 6.28 / 360 * 1000, servo->remap_deg_s, servo->remap_deg_e, deg_t);

    __HAL_TIM_SetCompare(servo->htim, servo->TIM_CHANNEL, (uint16_t)duty);
}
