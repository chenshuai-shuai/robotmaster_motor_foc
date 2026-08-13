/*
 * @Author: skybase
 * @Date: 2024-12-20 19:46:34
 * @LastEditors: skybase
 * @LastEditTime: 2025-01-16 10:17:46
 * @Description:  ᕕ(◠ڼ◠)ᕗ​
 * @FilePath: \luntui\BSP\Math\c_matrix.h
 */
#ifndef MATRIX_H
#define MATRIX_H

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include "tlsf.h"

#define MATRIX_TYPE float

#define MEMORY_POOL_SIZE 40960             // 定义内存池大小
extern char memory_pool[MEMORY_POOL_SIZE]; // 内存池
extern tlsf_t tlsf;                        // TLSF实例

typedef struct _Matrix
{
    /*Store Matrix
    存储矩阵*/
    int row;
    int column;
    MATRIX_TYPE *data;
} Matrix;

void creat_matrix_memory();

void marixToArray(MATRIX_TYPE *d, Matrix *p);
void arrayToMarix(Matrix *p, MATRIX_TYPE *d);
void printMatrix(const Matrix *matrix);

Matrix *create_matrix(int row, int column, MATRIX_TYPE *data);
void update_matrix_values(Matrix *mat, MATRIX_TYPE *new_data);
void matrix_cpy(Matrix *mat, Matrix *mat_);
Matrix *multiply_matrices(const Matrix *A, const Matrix *B);
void free_matrix(Matrix *m);
void assign_matrix(Matrix *dest, const Matrix *src);
Matrix *inverse_matrix(const Matrix *A);
Matrix *transpose_matrix(const Matrix *A);
Matrix *add_matrices(Matrix *mat_A, Matrix *mat_B);
Matrix *multiply_constant_with_matrix(MATRIX_TYPE constant, const Matrix *mat);

#endif
