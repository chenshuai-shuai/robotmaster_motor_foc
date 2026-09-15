/*
 * J8108_Task.h - 8108 关节电机接入测试任务（A 板 CAN1）
 */
#ifndef J8108_TASK_H
#define J8108_TASK_H

#include "j8108_action.h" /* 动作结果枚举（J8108_ActionResult_e）*/

#ifdef __cplusplus
extern "C"
{
#endif

void J8108_Task_Init(void);
J8108_ActionResult_e J8108_LastAction(uint32_t *ms_age); /* UI：最近动作结果（ms_age 可传 NULL） */
uint8_t J8108_IsHoldMode(void);                          /* UI：是否处于 HOLD 阻尼保持模式 */

#ifdef __cplusplus
}
#endif

#endif /* J8108_TASK_H */
