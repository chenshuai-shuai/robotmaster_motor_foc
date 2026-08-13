#ifndef DC_MOTOR_H_
#define DC_MOTOR_H_

#include "math.h"
#include "string.h"
#include "main.h"
#include "tim.h"

#include "pid.h"
#include "encoder.h"

typedef void (*motorControlCallback)(float spd);

typedef struct dc_motor_t
{
    uint8_t id;

    uint8_t motor_en;
    uint8_t polarity;
    float velocity_pwm;
    float velocity;
    float direction;

    uint8_t close_loop;

    pid_t *pid;
    inc_encoder_t *encoder;

    // !可调用的函数指针
    motorControlCallback motor_ctl;

} dc_motor_t;


void DCMotorInit(dc_motor_t *motor, uint8_t id, uint8_t polarity, inc_encoder_t *encoder, pid_t *pid);
void DCMotorSetPID(dc_motor_t *motor, uint32_t mode, uint32_t maxout, uint32_t intergral_limit, float kp, float ki, float kd);

void DCMotorEncoderUpdate(dc_motor_t *motor);
void DCMotorSetSpeedOpenLoop(dc_motor_t *motor, float speed, int isActive);
void DCMotorSetSpeedCloseLoop(dc_motor_t *motor, float speed, int isActive);

extern dc_motor_t motor_0;
extern dc_motor_t motor_1;
extern dc_motor_t motor_2;
extern dc_motor_t motor_3;
extern dc_motor_t motor_bat;

extern inc_encoder_t encoder_0;
extern inc_encoder_t encoder_1;
extern inc_encoder_t encoder_2;
extern inc_encoder_t encoder_3;

extern pid_t pid_0;
extern pid_t pid_1;
extern pid_t pid_2;
extern pid_t pid_3;

#endif
