/*
 * Disp_Task.h - 显示自检任务（把屏的"重型初始化 + 自检"从 main() 里搬出来）
 *
 * 用法：main() 里 Disp_Init() 之后调用 Disp_Task_Init() 即可（建任务，不阻塞）。
 * 详见 Disp_Task.c 顶部注释（为什么必须放在任务里）。
 */
#ifndef DISP_TASK_H
#define DISP_TASK_H

#include <stdint.h>

/* 栈 256 words（1KB）：本任务只调驱动内的初始化/自检 + 几条日志（snprintf），不递归不放大数组 */
#define DISP_TASK_STACK_WORDS (256u)
/* 优先级 2：低优先级 + DWT 忙等 → 不抢日志(5)/灯(5)/按键(4)/监控(3)/串口(5) 的 CPU，
 * 开机日志与流水灯因此不会被屏初始化挡住（这正是本任务存在的意义）。 */
#define DISP_TASK_PRIO (2u)

void Disp_Task_Init(void);

#endif /* DISP_TASK_H */
