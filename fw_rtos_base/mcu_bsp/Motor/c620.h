#ifndef C620_H_
#define C620_H_

#include "stdint.h"
#include "motor_def.h"
#include "can.h"
#include "bsp_can.h"

#define C620_GROUP_NUM 4

typedef struct c620_t
{
    uint8_t group_id; // 组id,用于区分不同的电机组
    uint8_t protocol_id;     //该组的id标识
    
		CANInstance *can_instance[4];
		Motor_Controller_struct motor_instnce[4];

}C620_group_Controller_t;

extern C620_group_Controller_t *C620_Group_instnce[C620_GROUP_NUM]; // c620实例数组

void C620_Group_Register(CAN_HandleTypeDef *hcan, uint8_t group_id, uint32_t protocol_id);
void C620_Group_Set_Current(uint8_t group_id);

#endif