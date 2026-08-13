/*
 * @Author: Ptisak
 * @Date: 2023-06-17 21:10:18
 * @LastEditors: skybase
 * @LastEditTime: 2025-03-03 03:36:57
 * @Version: Do not edit
 */

/* Includes ------------------------------------------------------------------*/
#include "pid.h"
#include <math.h>
#include "zf_common_headfile.h"

#define ABS(x) ((x > 0) ? (x) : (-x))

void abs_limit(float *a, float ABS_MAX)
{
    if (*a > ABS_MAX)
        *a = ABS_MAX;
    if (*a < -ABS_MAX)
        *a = -ABS_MAX;
}
/*--------------------------------------------------------------*/
static void pid_param_init(
    pid_t *pid,
    uint32_t mode,
    uint32_t maxout,
    uint32_t intergral_limit,
    float kp,
    float ki,
    float kd)
{

    pid->IntegralLimit = intergral_limit;
    pid->MaxOutput = maxout;
    pid->pid_mode = mode;

    pid->p = kp;
    pid->i = ki;
    pid->d = kd;
}
/*------------------------------------------------------------*/
void pid_reset(pid_t *pid, float kp, float ki, float kd)
{
    pid->p = kp;
    pid->i = ki;
    pid->d = kd;
}

/**
 *@bref. calculate delta PID and position PID
 *@param[in] set?? target
 *@param[in] real	measure
 */
float pid_calc(pid_t *pid, float get, float set)
{
    pid->get[NOW] = get;
    pid->set[NOW] = set;
    pid->err[NOW] = set - get; // set - measure
    if (pid->max_err != 0 && ABS(pid->err[NOW]) > pid->max_err)
        return 0;
    if (pid->deadband != 0 && ABS(pid->err[NOW]) < pid->deadband)
        return 0;

    if (pid->pid_mode == POSITION_PID) 
    {
        pid->pout = pid->p * pid->err[NOW];
        pid->iout += pid->i * pid->err[NOW];
        pid->dout = pid->d * (pid->err[NOW] - pid->err[LAST]);
        abs_limit(&(pid->iout), pid->IntegralLimit);
        pid->pos_out = pid->pout + pid->iout + pid->dout;
        abs_limit(&(pid->pos_out), pid->MaxOutput);
        pid->last_pos_out = pid->pos_out; // update last time
    }
    else if (pid->pid_mode == DELTA_PID) 
    {
        pid->pout = pid->p * (pid->err[NOW] - pid->err[LAST]);
        pid->iout = pid->i * pid->err[NOW];
        pid->dout = pid->d * (pid->err[NOW] - 2 * pid->err[LAST] + pid->err[LLAST]);

        abs_limit(&(pid->iout), pid->IntegralLimit);
        pid->delta_u = pid->pout + pid->iout + pid->dout;
        abs_limit(&(pid->delta_u), pid->IntegralLimit);

        pid->delta_out = pid->last_delta_out + pid->delta_u;
        abs_limit(&(pid->delta_out), pid->MaxOutput);
        pid->last_delta_out = pid->delta_out; // update last time
    }

    pid->err[LLAST] = pid->err[LAST];
    pid->err[LAST] = pid->err[NOW];
    pid->get[LLAST] = pid->get[LAST];
    pid->get[LAST] = pid->get[NOW];
    pid->set[LLAST] = pid->set[LAST];
    pid->set[LAST] = pid->set[NOW];
    pid->out = pid->pid_mode == POSITION_PID ? pid->pos_out : pid->delta_out;
    return pid->pid_mode == POSITION_PID ? pid->pos_out : pid->delta_out;
    //
}

/**
 *@bref. special calculate position PID @attention @use @gyro data!!
 *@param[in] set?? target
 *@param[in] real	measure
 */
float pid_sp_calc(pid_t *pid, float get, float set, float gyro)
{
    pid->get[NOW] = get;
    pid->set[NOW] = set;
    pid->err[NOW] = set - get; // set - measure

    if (pid->pid_mode == POSITION_PID) // λ��ʽp
    {
        pid->pout = pid->p * pid->err[NOW];
        if (fabs(pid->i) >= 0.001f)
            pid->iout += pid->i * pid->err[NOW];
        else
            pid->iout = 0;
        pid->dout = -pid->d * gyro / 100.0f;
        abs_limit(&(pid->iout), pid->IntegralLimit);
        pid->pos_out = pid->pout + pid->iout + pid->dout;
        abs_limit(&(pid->pos_out), pid->MaxOutput);
        pid->last_pos_out = pid->pos_out; // update last time
    }
    else if (pid->pid_mode == DELTA_PID) 
    {
                pid->pout = pid->p * (pid->err[NOW] - pid->err[LAST]);
                pid->iout = pid->i * pid->err[NOW];
                pid->dout = pid->d * (pid->err[NOW] - 2*pid->err[LAST] + pid->err[LLAST]);

                abs_limit(&(pid->iout), pid->IntegralLimit);
                pid->delta_u = pid->pout + pid->iout + pid->dout;
                pid->delta_out = pid->last_delta_out + pid->delta_u;
                abs_limit(&(pid->delta_out), pid->MaxOutput);
                pid->last_delta_out = pid->delta_out;	//update last time
    }

    pid->err[LLAST] = pid->err[LAST];
    pid->err[LAST] = pid->err[NOW];
    pid->get[LLAST] = pid->get[LAST];
    pid->get[LAST] = pid->get[NOW];
    pid->set[LLAST] = pid->set[LAST];
    pid->set[LAST] = pid->set[NOW];
    pid->out = pid->pid_mode == POSITION_PID ? pid->pos_out : pid->delta_out;

    return pid->pid_mode == POSITION_PID ? pid->pos_out : pid->delta_out;
    //
}


/// @brief 
/// @param pid 
/// @param mode 
/// @param maxout 
/// @param intergral_limit 
/// @param kp 
/// @param ki 
/// @param kd 
void PID_struct_init(
    pid_t *pid,
    uint32_t mode,
    uint32_t maxout,
    uint32_t intergral_limit,

    float kp,
    float ki,
    float kd)
{
    /*init function pointer*/
    pid->f_param_init = pid_param_init;
    pid->f_pid_reset = pid_reset;
    //	pid->f_cal_pid = pid_calc;
    //	pid->f_cal_sp_pid = pid_sp_calc;	//addition

    /*init pid param */
    pid->f_param_init(pid, mode, maxout, intergral_limit, kp, ki, kd);
}


