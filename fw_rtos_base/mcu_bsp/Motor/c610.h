/*
 * c610.h - RoboMaster C610 电调适配（M2006 电机）
 *
 * C610 与 C620 的 CAN 帧格式完全一致（0x200/0x1FF 命令帧、0x201~0x208 反馈帧），
 * 直接复用 C620 驱动（mcu_bsp/Motor/c620.c），仅电流量程不同：
 *   C610:  -10000 ~ 10000  ↔  -10A ~ +10A   （C620: -16384~16384 ↔ ±20A）
 * 反馈差异：C610 反馈帧 DATA[6] 为 Null（无电机温度），DATA[4:5] 为"实际输出转矩"原始值。
 *
 * 参考：D:\知识库\wiki-a-board\entities\c610.md / rm-motor-can-protocol.md
 */
#ifndef C610_H_
#define C610_H_

#include "c620.h"

/* C610 电流换算（A → CAN 原始值 / CAN 原始值 → A） */
#define C610_CURRENT_TO_RAW(amp)  ((int16_t)((amp) / 10.0f * 10000.0f))
#define C610_RAW_TO_CURRENT(raw)  (((raw) * 10.0f) / 10000.0f)

/* C610 电机组 = C620 驱动别名（帧格式相同） */
#define C610_Group_instnce       C620_Group_instnce
#define C610_Group_Register      C620_Group_Register
#define C610_Group_Set_Current   C620_Group_Set_Current

#endif /* C610_H_ */
