#ifndef __CONTROL_H
#define __CONTROL_H

#include "main.h"
#include "tim.h"
#include "servo.h"
namespace Control
{
    class Control_t
    {
        public:
        Control_t() = default;
        Control_t(Servo::Servo_base_t* servo_theta1,Servo::Servo_base_t* servo_theta2)
        {
            _servo_theta1 = servo_theta1;
            _servo_theta2 = servo_theta2;
            _x = 0.0f;
            _z = 0.0f;
           _yaw = 0.0f;

        }

        void controlUpdate(float x, float y, float yaw);
        void doCalcontrol();
        void doYawcontrol();


        float _x;
        float _z;
        float _yaw;

        Servo::Servo_base_t* _servo_theta1;
        Servo::Servo_base_t* _servo_theta2;
        Servo::Servo_base_t* _servoYAW;

    };
}



#endif
