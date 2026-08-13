#ifndef __KALMAN_H_
#define __KALMAN_H_

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// *便于使用的一阶卡尔曼滤波
typedef struct
{
    float estimate;          // 估计值
    float estimate_error;    // 估计误差
    float measurement_error; // 测量误差
    float process_noise;     // 过程噪声
} KalmanFilter_t;

void KalmanFilter_Init(KalmanFilter_t *kf, float initial_estimate, float initial_estimate_error, float measurement_error, float process_noise);
float KalmanFilter_Update(KalmanFilter_t *kf, float measurement);

// *拓展卡尔曼滤波(未验证)
typedef struct
{
    int state_dim; // 状态变量维度
    int obs_dim;   // 观测变量维度
    float *x;      // 状态估计值向量 [state_dim]
    float *P;      // 估计误差协方差矩阵 [state_dim * state_dim]
    float *Q;      // 过程噪声协方差矩阵 [state_dim * state_dim]
    float *R;      // 观测噪声协方差矩阵 [obs_dim * obs_dim]
    float *F;      // 状态转移矩阵 [state_dim * state_dim]
    float *H;      // 观测矩阵 [obs_dim * state_dim]
} KalmanFilter_Pro_t;

void KalmanFilter_Pro_Update(KalmanFilter_Pro_t *kf, float *z);
KalmanFilter_Pro_t *KalmanFilter_Pro_Init(int state_dim, int obs_dim);

#endif
