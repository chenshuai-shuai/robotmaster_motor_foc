#ifndef LED_TASK_H_
#define LED_TASK_H_

#include <stdint.h>   /* 契约头自足：任何组合都能单独 include（规范 R4） */

void Led_Task_Init(void);

/* 模块自检（非破坏性，只读 GPIO 配置回读）：0=未编译 1=OK 3=FAIL */
uint8_t Led_SelfTest(void);

#endif /* LED_TASK_H_ */
