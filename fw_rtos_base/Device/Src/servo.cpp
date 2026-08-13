#include "servo.h"
#include "FreeRTOS.h"
#include "task.h"
#include "math.h"
using namespace Servo;
#define rad_to_degree 57.30f
#define rate_to_pulse 3.7f

void Servo_base_t::control(float vel)
{
    float pulse = 0;
	float step=0;
	float temp_angle=0;
	step=(_target_angle>_current_angle)? ((_target_angle - _current_angle)/10.0f): ((_current_angle - _target_angle) / 10.0f);//步长
	
    _state=working;
	
    if (_target_angle > _current_angle)
    {

        for (temp_angle = _current_angle; temp_angle <= _target_angle; temp_angle += step)
        {
					if(_dir==pos_dir)
            pulse = (int)((temp_angle * 11.11f)+500);
					else if(_dir==reverse_dir)
						  pulse = (int)(((180-temp_angle) * 11.11f)+500);
            __HAL_TIM_SET_COMPARE(_htim, _channel, pulse);
            vTaskDelay(vel);
        }   
				   _current_angle = _target_angle;
    }
    else if(_target_angle < _current_angle)
    {
        for (temp_angle = _current_angle; temp_angle >= _target_angle;temp_angle-= step)
        {
          		if(_dir==pos_dir)
            pulse = (int)((temp_angle * 11.11f)+500);
					else if(_dir==reverse_dir)
						  pulse = (int)(((180-temp_angle) * 11.11f)+500);
            __HAL_TIM_SET_COMPARE(_htim, _channel, pulse);
            vTaskDelay(vel);
        }
				   _current_angle = _target_angle;                 
    }
		
    _state=finished;
}


// void Servo_base_t::control(float max_vel, float acceleration)
//{
//    float pulse = 0;
//    float current_vel = 0.0f;
//    float distance = _target_angle - _current_angle;
//    float direction = (distance > 0) ? 1.0f : -1.0f;
//    float remaining_distance = fabs(distance);
//    float deceleration_distance = 0.0f;
//    float step = 0.0f;
//    
//    _state = working;
//    
//    // 计算减速距离 (v² = u² + 2as)
//    deceleration_distance = (max_vel * max_vel) / (2 * acceleration);
//    
//    while (remaining_distance > 0.1f) // 设置一个小的阈值以避免无限循环
//    {
//        // 计算当前应移动的距离
//        if (remaining_distance > deceleration_distance)
//        {
//            // 加速或匀速阶段
//            if (current_vel < max_vel)
//            {
//                current_vel += acceleration * direction;
//                if (fabs(current_vel) > max_vel)
//                {
//                    current_vel = max_vel * direction;
//                }
//            }
//        }
//        else
//        {
//            // 减速阶段
//            if (fabs(current_vel) > 0.1f) // 避免过小的速度
//            {
//                current_vel -= acceleration * direction;
//            }
//            else
//            {
//                current_vel = 0.0f;
//            }
//        }
//        
//        // 计算步长 (基于当前速度)
//        step = current_vel;
//        
//        // 更新当前位置
//        _current_angle += step;
//        
//        // 确保不超出目标位置
//        if ((direction > 0 && _current_angle > _target_angle) || 
//            (direction < 0 && _current_angle < _target_angle))
//        {
//            _current_angle = _target_angle;
//        }
//        
//        // 计算并设置PWM脉冲
//        pulse = (int)((_current_angle * 11.1) + 450);
//        __HAL_TIM_SET_COMPARE(_htim, _channel, pulse);
//        
//        // 更新剩余距离
//        remaining_distance = fabs(_target_angle - _current_angle);
//        
//        // 固定时间延迟 (控制循环频率)
//        vTaskDelay(30); // 10ms周期，可根据需要调整
//    }
//    
//    _current_angle = _target_angle; // 确保最终位置准确
//    _state = finished;
//}