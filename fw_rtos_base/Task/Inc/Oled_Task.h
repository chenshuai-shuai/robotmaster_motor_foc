#ifndef OLED_TASK_H_
#define OLED_TASK_H_

#include "stdint.h"

void Oled_Task_Init(void);
uint8_t Oled_UiGetCursorRow(void); /* 当前光标行（动作层读取：决定长按作用在哪一行） */

#endif /* OLED_TASK_H_ */