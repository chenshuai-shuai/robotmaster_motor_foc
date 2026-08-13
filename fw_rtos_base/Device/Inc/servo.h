#ifndef __SERVO_H
#define __SERVO_H
#include "main.h"
// #include "task.h"

typedef enum
{
  working=0,
  finished  
} servo_state;

typedef enum
{
 pos_dir=0,
reverse_dir
}servo_dir;

namespace Servo
{
    class Servo_base_t
    {
    public:
        Servo_base_t() = default;
        Servo_base_t(TIM_HandleTypeDef *htim, uint32_t channel,servo_dir dir ,int zero_angle, float min_angle, float max_angle, float use_min_angle, float use_max_angle)
        {
            _htim = htim;
            _channel = channel;
            _target_angle = 0.0f;
            _current_angle = 0.0f;
            _velocity = 0.0f;
            _zero_angle = zero_angle;
            _max_angle = max_angle;
            _min_angle = min_angle;
            _use_min_angle = use_min_angle;
            _use_max_angle = use_max_angle;
            HAL_TIM_PWM_Start(_htim, _channel); 
					_dir=dir;
            __HAL_TIM_SET_COMPARE(_htim, _channel, (int)( _zero_angle*11.111f+500));
            _current_angle = _zero_angle;
        }

        // void control(float vel);
        void control(float vel);

        float _target_angle;
        float _current_angle;
        float _velocity;
        int _zero_angle;
        float _min_angle;
        float _max_angle;
        float _use_min_angle;
        float _use_max_angle;
        servo_state _state;
				servo_dir _dir;
    protected:
        TIM_HandleTypeDef *_htim;
        uint32_t _channel;
    };
} // namespace Servo

#endif
