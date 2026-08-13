
/*
 * @Author: skybase
 * @Date: 2024-11-05 17:47:38
 * @LastEditors: skybase
 * @LastEditTime: 2024-11-09 01:57:17
 * @Description:  ᕕ(◠ڼ◠)ᕗ​
 * @FilePath: \MDK-ARM\BSP\MOTOR\dc_motor.c
 */

/*
*该库用于驱动直流电机
*集成了pid库,encoder库
*可实现电机的开环和闭环驱动电机

>tip 该库中不包含数制的转换,转换将全部放在encoder库中

*/

#include "dc_motor.h"

// !用户对电机硬件接口的注册,包括pid,encoder,motor
// !用户只需要编辑以下接口,并定义电机对象就行

dc_motor_t motor_0;
dc_motor_t motor_1;
dc_motor_t motor_2;
dc_motor_t motor_3;
dc_motor_t motor_bat;

inc_encoder_t encoder_0;
inc_encoder_t encoder_1;
inc_encoder_t encoder_2;
inc_encoder_t encoder_3;

pid_t pid_0;
pid_t pid_1;
pid_t pid_2;
pid_t pid_3;

void userMotorInitBinding(dc_motor_t *motor)
{
    HAL_TIM_PWM_Start(&htim2, TIM_CHANNEL_3);
    HAL_TIM_PWM_Start(&htim2, TIM_CHANNEL_4);
    HAL_TIM_PWM_Start(&htim3, TIM_CHANNEL_3);
    HAL_TIM_PWM_Start(&htim3, TIM_CHANNEL_4);
    HAL_TIM_PWM_Start(&htim12, TIM_CHANNEL_1);
}

// !用户需要自行编写每个电机的驱动函数,并在下述userMotorCtlBinding中绑定该函数

void userMotorCtl_0(float spd)
{
    __HAL_TIM_SetCompare(&htim2, TIM_CHANNEL_3, (99 * fabsf(spd) / 100.0f));
    HAL_GPIO_WritePin(GPIOE, GPIO_PIN_15, (spd >= 0) ? 1 : 0);
}

void userMotorCtl_1(float spd)
{
    __HAL_TIM_SetCompare(&htim2, TIM_CHANNEL_4, (99 * fabsf(spd) / 100.0f));
    HAL_GPIO_WritePin(GPIOE, GPIO_PIN_14, (spd >= 0) ? 1 : 0);
}

void userMotorCtl_2(float spd)
{
    __HAL_TIM_SetCompare(&htim3, TIM_CHANNEL_3, (99 * fabsf(spd) / 100.0f));
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_5, (spd >= 0) ? 1 : 0);
}

void userMotorCtl_3(float spd)
{
    __HAL_TIM_SetCompare(&htim3, TIM_CHANNEL_4, (99 * fabsf(spd) / 100.0f));
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_4, (spd >= 0) ? 1 : 0);
}
void userMotorCtl_bat(float spd)
{
    __HAL_TIM_SetCompare(&htim12, TIM_CHANNEL_1, (99 * fabsf(spd) / 100.0f));
    HAL_GPIO_WritePin(GPIOH, GPIO_PIN_7, (spd >= 0) ? 1 : 0);
}

void userMotorCtlBinding(dc_motor_t *motor)
{
    if (motor->id == 0)
    {
        motor->motor_ctl = (void *)userMotorCtl_0;
    }
    if (motor->id == 1)
    {
        motor->motor_ctl = (void *)userMotorCtl_1;
    }
    if (motor->id == 2)
    {
        motor->motor_ctl = (void *)userMotorCtl_2;
    }
    if (motor->id == 3)
    {
        motor->motor_ctl = (void *)userMotorCtl_3;
    }
    if (motor->id == 4)
    {
        motor->motor_ctl = (void *)userMotorCtl_bat;
    }
}

//******************************
// !用户最后可调用的接口

void DCMotorInit(dc_motor_t *motor, uint8_t id, uint8_t polarity, inc_encoder_t *encoder, pid_t *pid)
{
    memset(motor, 0, sizeof(dc_motor_t));

    motor->motor_en = 1;
    motor->close_loop = 1;
    motor->id = id;
    motor->polarity = polarity;
    motor->encoder = encoder;
    motor->pid = pid;
    userMotorInitBinding(motor);
    userMotorCtlBinding(motor);

    // 初始化对应的编码器
    if (encoder != NULL)
        IncEncoderInit(encoder, id, 1);
}

void DCMotorSetPID(dc_motor_t *motor, uint32_t mode, uint32_t maxout, uint32_t intergral_limit, float kp, float ki, float kd)
{
    PID_struct_init(motor->pid, mode, maxout, intergral_limit, kp, ki, kd);
}

/// @brief 用于开环控制电机
/// @param motor
/// @param speed
/// @param isActive 如果为1,传入参数speed生效,否则无效
void DCMotorSetSpeedOpenLoop(dc_motor_t *motor, float speed, int isActive)
{
    if (isActive == 1)
    {
        motor->velocity_pwm = speed;
    }
    if (motor->polarity == 0)
    {
        motor->motor_ctl(motor->velocity_pwm);
    }
    else
    {
        motor->motor_ctl(-motor->velocity_pwm);
    }
}

void DCMotorEncoderUpdate(dc_motor_t *motor)
{
    // motor->encoder->update_tick = timbase;
    IncEncoderUpdate(motor->encoder);
}

/// @brief 用于闭环控制电机
/// @param motor
/// @param speed
/// @param isActive 如果为1,传入参数speed生效,否则无效
void DCMotorSetSpeedCloseLoop(dc_motor_t *motor, float speed, int isActive)
{
    // >传入的参数speed的优先级
    if (isActive == 1)
    {
        motor->velocity = speed;
    }
    if (motor->encoder != NULL)
    {
        DCMotorEncoderUpdate(motor);

        if (motor->pid != NULL)
        {
            DCMotorSetSpeedOpenLoop(motor, pid_calc(motor->pid, motor->velocity, motor->encoder->pulse_real), 1);
        }
    }
}
