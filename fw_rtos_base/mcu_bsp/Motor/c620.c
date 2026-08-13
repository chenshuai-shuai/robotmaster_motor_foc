#include "C620.h"

// !C620的ID从1开始
C620_group_Controller_t *C620_Group_instnce[C620_GROUP_NUM] = {NULL}; // c620实例数组
static int idx = 0;                                                          // 由于C620的id从1开始的特性,故而实例从下标1开始,共有第几组3508电机

void C620_Group_Get_Info(CANInstance *can_instance)
{
    switch (can_instance->rx_id)
    {
    case 0x201:
    {
        ((C620_group_Controller_t *)(can_instance->id))->motor_instnce[0].get.deg_pos = (can_instance->rx_buff[0] << 8) + can_instance->rx_buff[1];
        ((C620_group_Controller_t *)(can_instance->id))->motor_instnce[0].get.velocity = (can_instance->rx_buff[2] << 8) + can_instance->rx_buff[3];
        ((C620_group_Controller_t *)(can_instance->id))->motor_instnce[0].get.current = (can_instance->rx_buff[4] << 8) + can_instance->rx_buff[5];
        break;
    }
    case 0x202:
    {
        ((C620_group_Controller_t *)(can_instance->id))->motor_instnce[1].get.deg_pos = (can_instance->rx_buff[0] << 8) + can_instance->rx_buff[1];
        ((C620_group_Controller_t *)(can_instance->id))->motor_instnce[1].get.velocity = (can_instance->rx_buff[2] << 8) + can_instance->rx_buff[3];
        ((C620_group_Controller_t *)(can_instance->id))->motor_instnce[1].get.current = (can_instance->rx_buff[4] << 8) + can_instance->rx_buff[5];
        break;
    }
    case 0x203:
    {
        ((C620_group_Controller_t *)(can_instance->id))->motor_instnce[2].get.deg_pos = (can_instance->rx_buff[0] << 8) + can_instance->rx_buff[1];
        ((C620_group_Controller_t *)(can_instance->id))->motor_instnce[2].get.velocity = (can_instance->rx_buff[2] << 8) + can_instance->rx_buff[3];
        ((C620_group_Controller_t *)(can_instance->id))->motor_instnce[2].get.current = (can_instance->rx_buff[4] << 8) + can_instance->rx_buff[5];
        break;
    }
    case 0x204:
    {
        ((C620_group_Controller_t *)(can_instance->id))->motor_instnce[3].get.deg_pos = (can_instance->rx_buff[0] << 8) + can_instance->rx_buff[1];
        ((C620_group_Controller_t *)(can_instance->id))->motor_instnce[3].get.velocity = (can_instance->rx_buff[2] << 8) + can_instance->rx_buff[3];
        ((C620_group_Controller_t *)(can_instance->id))->motor_instnce[3].get.current = (can_instance->rx_buff[4] << 8) + can_instance->rx_buff[5];
        break;
    }
    case 0x205:
    {
        ((C620_group_Controller_t *)(can_instance->id))->motor_instnce[0].get.deg_pos = (can_instance->rx_buff[0] << 8) + can_instance->rx_buff[1];
        ((C620_group_Controller_t *)(can_instance->id))->motor_instnce[0].get.velocity = (can_instance->rx_buff[2] << 8) + can_instance->rx_buff[3];
        ((C620_group_Controller_t *)(can_instance->id))->motor_instnce[0].get.current = (can_instance->rx_buff[4] << 8) + can_instance->rx_buff[5];
        break;
    }
    case 0x206:
    {
        ((C620_group_Controller_t *)(can_instance->id))->motor_instnce[1].get.deg_pos = (can_instance->rx_buff[0] << 8) + can_instance->rx_buff[1];
        ((C620_group_Controller_t *)(can_instance->id))->motor_instnce[1].get.velocity = (can_instance->rx_buff[2] << 8) + can_instance->rx_buff[3];
        ((C620_group_Controller_t *)(can_instance->id))->motor_instnce[1].get.current = (can_instance->rx_buff[4] << 8) + can_instance->rx_buff[5];
        break;
    }
    case 0x207:
    {
        ((C620_group_Controller_t *)(can_instance->id))->motor_instnce[2].get.deg_pos = (can_instance->rx_buff[0] << 8) + can_instance->rx_buff[1];
        ((C620_group_Controller_t *)(can_instance->id))->motor_instnce[2].get.velocity = (can_instance->rx_buff[2] << 8) + can_instance->rx_buff[3];
        ((C620_group_Controller_t *)(can_instance->id))->motor_instnce[2].get.current = (can_instance->rx_buff[4] << 8) + can_instance->rx_buff[5];
        break;
    }
    case 0x208:
    {
        ((C620_group_Controller_t *)(can_instance->id))->motor_instnce[3].get.deg_pos = (can_instance->rx_buff[0] << 8) + can_instance->rx_buff[1];
        ((C620_group_Controller_t *)(can_instance->id))->motor_instnce[3].get.velocity = (can_instance->rx_buff[2] << 8) + can_instance->rx_buff[3];
        ((C620_group_Controller_t *)(can_instance->id))->motor_instnce[3].get.current = (can_instance->rx_buff[4] << 8) + can_instance->rx_buff[5];
        break;
    }
    }
}
/**
 * @brief
 *
 * @param hcan ,表示can1还是can2
 * @param group_id, 表示can1的第几组c620电机
 * @param protocol_id 0x200 or 0x1FF
 */
void C620_Group_Register(CAN_HandleTypeDef *hcan, uint8_t group_id, uint32_t protocol_id)
{
    int i;

    C620_group_Controller_t *C620_group_s = (C620_group_Controller_t *)malloc(sizeof(C620_group_Controller_t));
    C620_group_s->group_id = group_id;
    C620_group_s->protocol_id = protocol_id;
    CAN_Init_Config_s can_instance_config={0};
    memset(C620_group_s, 0, sizeof(C620_group_Controller_t));
    can_instance_config.can_handle = hcan;
    can_instance_config.tx_id = protocol_id;
    can_instance_config.can_module_callback = C620_Group_Get_Info;
    can_instance_config.id = C620_group_s;
    
    // 四个电机注册
    for(i=0;i<=3;i++)
    {
        can_instance_config.rx_id = (protocol_id == 0x200) ? 0x200 + i : 0x204 + i;
        C620_group_s->can_instance[i] = CANRegister(&can_instance_config);
    }
    
    C620_Group_instnce[idx++] = C620_group_s;
}

static void C620_Group_base_current(uint8_t group_id)
{
    int i;
    for(i=0;i<=3;i++)
    {
        if (MOTOR_STOP == C620_Group_instnce[group_id]->motor_instnce[i].motor_cofig.motor_enable_flag)
        {
            C620_Group_instnce[group_id]->motor_instnce[i].set.current = 0;
        }
        else
        {
            if (MOTOR_DIRECTION_REVERSE == C620_Group_instnce[group_id]->motor_instnce[i].motor_cofig.motor_reverse_flag)
            {
                MOTOR_VAR_REVERSE(C620_Group_instnce[group_id]->motor_instnce[i].set.current);
            }
        }
    }
}

void C620_Group_Set_Current(uint8_t group_id)
{
    C620_Group_base_current(group_id);
    uint8_t motor_current_data[8] = {0};
    int i;

    for(i=0;i<=5;i+=2)
    {
        motor_current_data[i] = (int16_t)C620_Group_instnce[group_id]->motor_instnce[i/2].set.current >> 8 ;
        motor_current_data[i+1] = (int16_t)C620_Group_instnce[group_id]->motor_instnce[i/2].set.current ;
    }

    memcpy(C620_Group_instnce[group_id]->can_instance[0]->tx_buff, motor_current_data,8);
	   CANTransmit(C620_Group_instnce[group_id]->can_instance[0]);	
}
