#include "data_filter.h"
#include "stdlib.h"

//#define SAMPLE_RATE 1000.0f // 假设采样率为1000 Hz
//#define CUTOFF_FREQ 5.0f	// 设置截止频率为5 Hz

//// 一阶RC低通滤波器
//void LowPassFilter_RC(float Vi, float *Vo)
//{
//	static float Vo_p = 0.0f; // 前一时刻的输出信号，静态变量用于保存历史值
//	float RC, Cof1, Cof2;

//	// 计算RC时间常数
//	RC = 1.0f / (2.0f * M_PI * CUTOFF_FREQ);			 // RC = 1 / (2 * pi * cutoff_freq)
//	Cof1 = 1.0f / (1.0f + RC * SAMPLE_RATE);			 // 系数1
//	Cof2 = RC * SAMPLE_RATE / (1.0f + RC * SAMPLE_RATE); // 系数2

//	// 低通滤波器公式
//	*Vo = Cof1 * Vi + Cof2 * Vo_p;

//	// 更新前一时刻的输出信号
//	Vo_p = *Vo;
//}

///**
// * @brief  高通滤波器
// * @author
// * @param  in-输入数据 out-滤波输出数据
// * @return void
// */
//void high_pass_filter(float in, float *out)
//{
//	float rc, coff;
//	static float in_p, out_p;

//	rc = 1.0 / 2.0 / Pi / HP_CUT_FRQ;
//	coff = rc / (rc + 1 / SAMPLE_RATE);
//	*out = (in - in_p + out_p) * coff;

//	out_p = *out;
//	in_p = in;
//}


/**
 * @brief 窗口滤波函数
 * @note 使用数组逐个移位的方式进行数据更新,时间复杂度为O(N)
 * @todo 后续将使用环形队列进行优化,将时间复杂度缩减至O(1)
 * @param len 窗口矩阵的个数
 */
float window_Filter(float data, float *buf, uint8_t len)
{
	uint8_t i;
	float sum = 0;

	for (i = 1; i < len; i++)
	{
		buf[i - 1] = buf[i];
	}
	buf[len - 1] = data;

	for (i = 0; i < len; i++)
	{
		sum += buf[i];
	}

	sum /= len;

	return sum;
}

