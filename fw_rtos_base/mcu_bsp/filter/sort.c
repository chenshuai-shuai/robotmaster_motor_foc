#include "sort.h"

/**
 * @brief 冒泡排序
 * @note 使用数组逐个移位的方式进行数据更新, 时间复杂度为 O(N^2)
 *
 * @param arr 待排序的数组
 * @param len 窗口矩阵的个数
 * @param order  @ref SORT_ORDER
 */
void bubble_Sort(float *arr, int len, SORT_ORDER order) 
{
    float temp;
    int i, j;

    for (i = 0; i < len - 1; i++) /* 外循环为排序趟数，len个数进行len-1趟 */
    {
        for (j = 0; j < len - 1 - i; j++)
        {
            // 根据order决定比较的顺序
            if ((order == 0 && arr[j] > arr[j + 1]) || (order == 1 && arr[j] < arr[j + 1]))
            {
                /* 如果是升序且 arr[j] > arr[j + 1]，或者是降序且 arr[j] < arr[j + 1]，则交换 */
                temp = arr[j];
                arr[j] = arr[j + 1];
                arr[j + 1] = temp;
            }
        }
    }
}