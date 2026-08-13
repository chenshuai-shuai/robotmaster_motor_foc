/*
 * @Author: skybase
 * @Date: 2024-12-20 19:45:54
 * @LastEditors: skybase
 * @LastEditTime: 2025-01-14 03:50:53
 * @Description:  ᕕ(◠ڼ◠)ᕗ​
 * @FilePath: \luntui\BSP\Math\c_matrix.c
 */
#include "c_matrix.h"
#include "bsp_log.h"

char memory_pool[MEMORY_POOL_SIZE]; // 内存池
tlsf_t tlsf;                        // TLSF 句柄

void creat_matrix_memory()
{
    tlsf = tlsf_create_with_pool(memory_pool, MEMORY_POOL_SIZE);
}

void marixToArray(MATRIX_TYPE *d, Matrix *p)
{
    memcpy(d, p->data, p->row * p->column * sizeof(MATRIX_TYPE));
}

void arrayToMarix(Matrix *p, MATRIX_TYPE *d)
{
    memcpy(p->data, d, p->row * p->column * sizeof(MATRIX_TYPE));
}

void printMatrix(const Matrix *matrix)
{
    if (!matrix || !matrix->data)
    {
        LOGERROR("Invalid matrix or matrix data.\n");
        return;
    }

    LOGINFO("Matrix (%d x %d):\n", matrix->row, matrix->column);
    for (int i = 0; i < matrix->row; i++)
    {
        for (int j = 0; j < matrix->column; j++)
        {
            // 使用偏移量计算元素位置
            LOG_PRINT("%f ", matrix->data[i * matrix->column + j]);
        }
        LOG_PRINT("\n"); // 换行
    }
}

Matrix *create_matrix(int row, int column, MATRIX_TYPE *data)
{
    // Generate a new Matrix (struct).
    Matrix *_mat = (Matrix *)tlsf_malloc(tlsf, sizeof(Matrix));
    if (!_mat)
    {
        // printf("Memory allocation for matrix structure failed.\n");
        return NULL;
    }

    _mat->row = row;
    _mat->column = column;
    int size = _mat->row * _mat->column;

    // Allocate memory for the matrix data
    _mat->data = (MATRIX_TYPE *)tlsf_malloc(tlsf, size * sizeof(MATRIX_TYPE));
    if (!_mat->data)
    {
        // printf("Memory allocation for matrix data failed.\n");
        tlsf_free(tlsf, _mat);
        return NULL;
    }

    // If data is provided, copy it to the newly allocated matrix data
    if (data != NULL)
    {
        memcpy(_mat->data, data, size * sizeof(MATRIX_TYPE));
    }
    else
    {
        // Initialize the matrix data to zero if no data is provided
        for (int i = 0; i < size; i++)
        {
            _mat->data[i] = 0;
        }
    }

    return _mat;
}

void update_matrix_values(Matrix *mat, MATRIX_TYPE *new_data)
{
    int size = mat->row * mat->column;
    memcpy(mat->data, new_data, size * sizeof(MATRIX_TYPE));
}

void assign_matrix(Matrix *dest, const Matrix *src)
{
    // Check if the destination and source matrices have the same dimensions
    if (dest->row != src->row || dest->column != src->column)
    {
        // printf("Matrix dimensions do not match for assignment.\n");
        return; // Exit the function if dimensions do not match
    }

    // Copy the data from source to destination
    memcpy(dest->data, src->data, dest->row * dest->column * sizeof(MATRIX_TYPE));
}

// Function to free the memory allocated for the matrix
void free_matrix(Matrix *m)
{
    tlsf_free(tlsf, m->data);
    tlsf_free(tlsf, m);
}

// Function to perform matrix multiplication
Matrix *multiply_matrices(const Matrix *A, const Matrix *B)
{
    // Check if multiplication is possible: columns of A must be equal to rows of B
    if (A->column != B->row)
    {
        // printf("Matrix multiplication is not possible. The number of columns of A must equal the number of rows of B.\n");
        return NULL;
    }

    // Create a result matrix with dimensions (A->row) x (B->column)
    Matrix *C = create_matrix(A->row, B->column, NULL);

    // Perform matrix multiplication: C = A * B
    for (int i = 0; i < A->row; i++)
    {
        for (int j = 0; j < B->column; j++)
        {
            C->data[i * B->column + j] = 0; // Initialize the element
            for (int k = 0; k < A->column; k++)
            {
                C->data[i * B->column + j] += A->data[i * A->column + k] * B->data[k * B->column + j];
            }
        }
    }
    return C;
}

// Function to perform matrix inversion using Gauss-Jordan elimination
Matrix *inverse_matrix(const Matrix *A)
{
    // Check if the matrix is square
    if (A->row != A->column)
    {
        // printf("Matrix must be square to compute its inverse.\n");
        return NULL;
    }

    int n = A->row;
    // Create a matrix to hold the inverse, initially set to the identity matrix
    Matrix *inv = create_matrix(n, n, NULL);
    for (int i = 0; i < n; i++)
    {
        for (int j = 0; j < n; j++)
        {
            inv->data[i * n + j] = (i == j) ? 1.0f : 0.0f; // Identity matrix
        }
    }

    // Create a copy of A to perform the row operations
    Matrix *temp = create_matrix(n, n, A->data);

    // Perform Gauss-Jordan elimination
    for (int i = 0; i < n; i++)
    {
        // Search for the largest element in the current column
        int max_row = i;
        for (int k = i + 1; k < n; k++)
        {
            if (fabs(temp->data[k * n + i]) > fabs(temp->data[max_row * n + i]))
            {
                max_row = k;
            }
        }

        // Swap the current row with the row with the largest element
        if (max_row != i)
        {
            for (int j = 0; j < n; j++)
            {
                MATRIX_TYPE tmp = temp->data[i * n + j];
                temp->data[i * n + j] = temp->data[max_row * n + j];
                temp->data[max_row * n + j] = tmp;

                tmp = inv->data[i * n + j];
                inv->data[i * n + j] = inv->data[max_row * n + j];
                inv->data[max_row * n + j] = tmp;
            }
        }

        // Normalize the pivot row
        MATRIX_TYPE pivot = temp->data[i * n + i];
        if (pivot == 0)
        {
            // printf("Matrix is singular and cannot be inverted.\n");
            free_matrix(inv);
            free_matrix(temp);
            return NULL;
        }

        for (int j = 0; j < n; j++)
        {
            temp->data[i * n + j] /= pivot;
            inv->data[i * n + j] /= pivot;
        }

        // Eliminate all other entries in the current column
        for (int k = 0; k < n; k++)
        {
            if (k != i)
            {
                MATRIX_TYPE factor = temp->data[k * n + i];
                for (int j = 0; j < n; j++)
                {
                    temp->data[k * n + j] -= factor * temp->data[i * n + j];
                    inv->data[k * n + j] -= factor * inv->data[i * n + j];
                }
            }
        }
    }

    // Free the temporary matrix
    free_matrix(temp);

    return inv;
}

// Function to calculate the transpose of a matrix
Matrix *transpose_matrix(const Matrix *A)
{
    // Create a new matrix for the transpose with swapped rows and columns
    Matrix *B = create_matrix(A->column, A->row, NULL);

    // Perform the transpose by swapping rows and columns
    for (int i = 0; i < A->row; i++)
    {
        for (int j = 0; j < A->column; j++)
        {
            B->data[j * A->row + i] = A->data[i * A->column + j];
        }
    }

    return B;
}

// Function to add two matrices
Matrix *add_matrices(Matrix *mat_A, Matrix *mat_B)
{
    // Check if the dimensions of the two matrices match
    if (mat_A->row != mat_B->row || mat_A->column != mat_B->column)
    {
        // printf("Matrix dimensions must be the same for addition.\n");
        return NULL; // Return NULL if the dimensions don't match
    }

    // Create a result matrix with the same dimensions
    Matrix *result = create_matrix(mat_A->row, mat_A->column, NULL);

    // Perform matrix addition element by element
    for (int i = 0; i < mat_A->row; i++)
    {
        for (int j = 0; j < mat_A->column; j++)
        {
            result->data[i * mat_A->column + j] = mat_A->data[i * mat_A->column + j] + mat_B->data[i * mat_A->column + j];
        }
    }

    return result;
}

Matrix *multiply_constant_with_matrix(MATRIX_TYPE constant, const Matrix *mat)
{
    Matrix *result = create_matrix(mat->row, mat->column, NULL);
    for (int i = 0; i < mat->row; i++)
        for (int j = 0; j < mat->column; j++)
            result->data[i * mat->column + j] = constant * mat->data[i * mat->column + j];
    return result;
}

void matrix_cpy(Matrix *mat, Matrix *mat_)
{
    memcpy(mat->data, mat_->data, mat->row * mat->column * sizeof(MATRIX_TYPE));
}