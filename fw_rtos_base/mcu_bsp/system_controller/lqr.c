#include "lqr.h"
#include "bsp_log.h"

void LQR_Init(LQR_struct *_instance, int x_dim, int u_dim)
{
    memset(_instance, 0, sizeof(LQR_struct));

    _instance->mat_K = (Matrix_type *)malloc(sizeof(Matrix_type) * u_dim * x_dim);
    _instance->mat_X = (Matrix_type *)malloc(sizeof(Matrix_type) * x_dim);
    _instance->mat_U = (Matrix_type *)malloc(sizeof(Matrix_type) * u_dim);

    _instance->x_dim = x_dim;
    _instance->u_dim = u_dim;
}

/**
 * @brief
 *
 * @param _instance
 * @param x_val 满足该实例维数的一位n*1维数组
 * @param u_val
 */
Matrix_type *LQR_Calculate(LQR_struct *_instance, Matrix_type *x_val, Matrix_type *x_set_val)
{
    memcpy(_instance->mat_X, x_val, sizeof(Matrix_type) * _instance->x_dim);

    for (int i = 0; i < _instance->u_dim; i++)
    {
        _instance->mat_U[i] = 0;

        for (int j = 0; j < _instance->x_dim; j++)
        {
            _instance->mat_U[i] += (_instance->mat_K[i * _instance->x_dim + j] * (x_set_val[j] - x_val[j]));
        }
    }
    return _instance->mat_U;
}
