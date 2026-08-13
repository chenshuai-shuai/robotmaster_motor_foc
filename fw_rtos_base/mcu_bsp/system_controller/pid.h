/*
 * @Author: Ptisak
 * @Date: 2023-06-17 21:10:18
 * @LastEditors: skybase
 * @LastEditTime: 2025-02-28 04:16:04
 * @Version: Do not edit
 */
/******************************************************************************
/// @brief
/// @copyright Copyright (c) 2017 <dji-innovations, Corp. RM Dept.>
/// @license MIT License
/// Permission is hereby granted, free of charge, to any person obtaining a copy
/// of this software and associated documentation files (the "Software"), to deal
/// in the Software without restriction,including without limitation the rights
/// to use, copy, modify, merge, publish, distribute, sublicense,and/or sell
/// copies of the Software, and to permit persons to whom the Software is furnished
/// to do so,subject to the following conditions:
///
/// The above copyright notice and this permission notice shall be included in
/// all copies or substantial portions of the Software.
///
/// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
/// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
/// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
/// THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
/// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
/// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
/// THE SOFTWARE.
*******************************************************************************/
#ifndef __pid_H
#define __pid_H
/* Includes ------------------------------------------------------------------*/
#include "stdint.h"

enum
{
    LLAST = 0,
    LAST = 1,
    NOW = 2,

    POSITION_PID,
    DELTA_PID,
};
typedef struct __pid_t
{
    float p;
    float i;
    float d;

    float set[3]; 
    float get[3]; 
    float err[3]; 

    float pout; 
    float iout; 
    float dout; 

    float pos_out;      
    float last_pos_out; 
    float delta_u;      
    float delta_out;    
    float last_delta_out;
    float out;

    float max_err;
    float deadband; // err < deadband return
    uint32_t pid_mode;
    uint32_t MaxOutput;     
    uint32_t IntegralLimit; 

    void (*f_param_init)(struct __pid_t *pid, 
                         uint32_t pid_mode,
                         uint32_t maxOutput,
                         uint32_t integralLimit,
                         float p,
                         float i,
                         float d);
    void (*f_pid_reset)(struct __pid_t *pid, float p, float i, float d); 

} pid_t;

void PID_struct_init(
    pid_t *pid,
    uint32_t mode,
    uint32_t maxout,
    uint32_t intergral_limit,

    float kp,
    float ki,
    float kd);

void pid_reset(pid_t *pid, float kp, float ki, float kd);
float pid_calc(pid_t *pid, float get, float set);
float pid_sp_calc(pid_t *pid, float get, float set, float gyro);

#endif
