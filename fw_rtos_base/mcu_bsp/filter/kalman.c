#include "kalman.h"

/**
 * @brief   卡尔曼滤波初始化
 */
void KalmanFilter_Init(KalmanFilter_t *kf, float initial_estimate, float initial_estimate_error, float measurement_error, float process_noise)
{
    kf->estimate = initial_estimate;
    kf->estimate_error = initial_estimate_error;
    kf->measurement_error = measurement_error;
    kf->process_noise = process_noise;
}

float KalmanFilter_Update(KalmanFilter_t *kf, float measurement)
{
    // 预测阶段
    kf->estimate_error += kf->process_noise;

    // 更新阶段
    float kalman_gain = kf->estimate_error / (kf->estimate_error + kf->measurement_error);
    kf->estimate += kalman_gain * (measurement - kf->estimate);
    kf->estimate_error *= (1 - kalman_gain);

    return kf->estimate;
}

// 矩阵乘法 C = A * B, A[row1 * col1], B[col1 * col2], C[row1 * col2]
static void Matrix_Mul(float *A, float *B, float *C, int row1, int col1, int col2)
{
    memset(C, 0, sizeof(float) * row1 * col2);
    for (int i = 0; i < row1; i++)
    {
        for (int j = 0; j < col2; j++)
        {
            for (int k = 0; k < col1; k++)
            {
                C[i * col2 + j] += A[i * col1 + k] * B[k * col2 + j];
            }
        }
    }
}

// 矩阵加法 C = A + B
static void Matrix_Add(float *A, float *B, float *C, int row, int col)
{
    for (int i = 0; i < row * col; i++)
    {
        C[i] = A[i] + B[i];
    }
}

// 矩阵减法 C = A - B
static void Matrix_Sub(float *A, float *B, float *C, int row, int col)
{
    for (int i = 0; i < row * col; i++)
    {
        C[i] = A[i] - B[i];
    }
}

KalmanFilter_Pro_t *KalmanFilter_Pro_Init(int state_dim, int obs_dim)
{
    KalmanFilter_Pro_t *kf = (KalmanFilter_Pro_t *)malloc(sizeof(KalmanFilter_Pro_t));
    if (!kf)
        return NULL;

    kf->state_dim = state_dim;
    kf->obs_dim = obs_dim;

    // 动态分配数组
    kf->x = (float *)calloc(state_dim, sizeof(float));
    kf->P = (float *)calloc(state_dim * state_dim, sizeof(float));
    kf->Q = (float *)calloc(state_dim * state_dim, sizeof(float));
    kf->R = (float *)calloc(obs_dim * obs_dim, sizeof(float));
    kf->F = (float *)calloc(state_dim * state_dim, sizeof(float));
    kf->H = (float *)calloc(obs_dim * state_dim, sizeof(float));

    // 初始化 P 为单位矩阵
    for (int i = 0; i < state_dim; i++)
    {
        kf->P[i * state_dim + i] = 1.0f;
    }

    return kf;
}
void KalmanFilter_Pro_Update(KalmanFilter_Pro_t *kf, float *z)
{
    int n = kf->state_dim;
    int m = kf->obs_dim;

    float *x_pred = (float *)calloc(n, sizeof(float));
    float *P_pred = (float *)calloc(n * n, sizeof(float));
    float *y = (float *)calloc(m, sizeof(float));
    float *S = (float *)calloc(m * m, sizeof(float));
    float *K = (float *)calloc(n * m, sizeof(float));
    float *H_P_pred = (float *)calloc(m * n, sizeof(float));

    // 预测 x_pred = F * x
    Matrix_Mul(kf->F, kf->x, x_pred, n, n, 1);

    // 预测 P_pred = F * P * F^T + Q
    Matrix_Mul(kf->F, kf->P, P_pred, n, n, n);
    Matrix_Add(P_pred, kf->Q, P_pred, n, n);

    // 计算创新 y = z - H * x_pred
    Matrix_Mul(kf->H, x_pred, y, m, n, 1);
    Matrix_Sub(z, y, y, m, 1);

    // 计算 S = H * P_pred * H^T + R
    Matrix_Mul(kf->H, P_pred, H_P_pred, m, n, n);
    Matrix_Mul(H_P_pred, kf->H, S, m, n, m);
    Matrix_Add(S, kf->R, S, m, m);

    // 计算 K = P_pred * H^T * S^-1（假设 S 是标量）
    float S_inv = 1.0 / S[0];
    for (int i = 0; i < n; i++)
    {
        K[i] = P_pred[i * n] * S_inv;
    }

    // 更新状态 x = x_pred + K * y
    for (int i = 0; i < n; i++)
    {
        kf->x[i] = x_pred[i] + K[i] * y[0];
    }

    // 释放临时变量
    free(x_pred);
    free(P_pred);
    free(y);
    free(S);
    free(K);
    free(H_P_pred);
}
