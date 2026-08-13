/*
 * @Author: skybase
 * @Date: 2025-04-29 21:23:08
 * @LastEditors: skybase
 * @LastEditTime: 2025-04-30 18:12:27
 * @Description:  ᕕ(◠ڼ◠)ᕗ​
 * @FilePath: \2025_RC_JUMP\mcu_bsp\Motor\dm_j4310.h
 */
#ifndef DM_J4310_H
#define DM_J4310_H

#include "stdint.h"
#include "motor_def.h"
#include "can.h"
#include "bsp_can.h"
#define DM_J4310_NUM 4 // dm_j4310实例数量,根据实际需要修改

typedef enum
{
    mit_mode = 0,
    pos_vel_mode = 1,
    vel_mode = 2,
} DM_J4310_Working_MODE_e;
typedef enum
{
    overpressure = 8,
    undervoltage = 9,
    overcurrent = (int16_t)'A',
    MOS_OT = (int16_t)'B',
    Rotor_OT = (int16_t)'C',
    Relate_lose = (int16_t)'D',
    overload = (int16_t)'E',

} erro__type_e;

typedef struct
{
    uint16_t Master_id; // 控制器id，取can_id的低8位
    erro__type_e Erro;
    // 速度和位置都在 motor_instnce->get里面
    float Torque;
    float T_Mos;
    float T_Rotor;
} DM_Info_t;

typedef struct
{
    float P_des;
    float V_des;
    float Kp;
    float Kd;
    float Torque;
} DM_Controller_Struct;

typedef struct dm_j4310_t
{
    uint32_t protocol_id;         // 协议id
    uint32_t mode_trans_id;       // 工作模式id,用于区分不同的工作模式
    DM_J4310_Working_MODE_e mode; // 电机工作模式
    CANInstance *can_instance;
    DM_Info_t dm_imfo_instance;                  // dm电机特殊返回数据(信息类)
    DM_Controller_Struct dm_controller_instance; // dm电机控制器数据
    Motor_Controller_struct motor_instnce;
} DM_J4310_Controller_t;

void DM_J4310_Get_Info(CANInstance *can_instance);
DM_J4310_Controller_t *DM_4310_Register(CAN_HandleTypeDef *hcan, uint32_t protocol_id, uint32_t mst_id, uint16_t w_mode);
void DM_Change_Mode(DM_J4310_Controller_t *dm_j4310_instance, uint8_t to_mode);
void Enable_DM(DM_J4310_Controller_t *dm_j4310_instance);
void Control_DM(DM_J4310_Controller_t *dm_j4310_instance);

#endif
