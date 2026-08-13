#include "dm_j4310.h"

static DM_J4310_Controller_t *DM_J4310_instnce[DM_J4310_NUM] = {NULL}; // dm_j4310实例数组
static int dm_idx = 0;                                                 // dm_j4310实例索引,每次有新的模块注册会自增

uint16_t FloatToU16(float value, float min, float max, int bits)
{
    float range = max - min;
    return (uint16_t)((value - min) * ((1 << bits) - 1) / range);
}

int16_t FloatToI16(float value, float min, float max, int bits)
{
    float range = max - min;
    return (int16_t)((value - min) * ((1 << bits) - 1) / range - (1 << (bits - 1)));
}

void DM_J4310_Get_Info(CANInstance *can_instance)
{
    uint8_t data[8] = {0};
    memcpy(data, can_instance->rx_buff, 8);

    // 1. 解析电机ID和故障码
    uint8_t motor_id = data[0] & 0x0F;          // 低4位为ID
    uint8_t error_code = (data[0] >> 4) & 0x0F; // 高4位为ERR
                                                // 2. 解析位置（16位有符号整数 -> 弧度）
    int16_t pos_raw = (data[1] << 8) | data[2];
    float position = (float)pos_raw * (2.0f * 30.0f / 65536.0f); // 示例：满量程±π rad

    // 3. 解析速度（12位有符号整数 -> rad/s）
    int16_t vel_raw = (int16_t)((data[3] << 4) | ((data[4] >> 4) & 0x0F));
    vel_raw = (vel_raw << 4) >> 4;                                        // 符号扩展（12位->16位）
    float velocity = (float)vel_raw * (200.0f * 30.0f / 60.0f / 2048.0f); // 根据电机参数调整

    // 4. 解析扭矩（12位有符号整数 -> N·m）
    int16_t torque_raw = (int16_t)(((data[4] & 0x0F) << 8) | data[5]);
    torque_raw = (torque_raw << 4) >> 4;                 // 符号扩展（12位->16位）
    float torque = (float)torque_raw * (7.0f / 2048.0f); // 示例：满量程±7 N·m

    // 5. 解析温度
    uint8_t t_mos = data[6];   // MOS温度（℃）
    uint8_t t_rotor = data[7]; // 线圈温度（℃）

    ((DM_J4310_Controller_t *)(can_instance->id))->motor_instnce.get.deg_pos = position;
    ((DM_J4310_Controller_t *)(can_instance->id))->motor_instnce.get.velocity = vel_raw;
    ((DM_J4310_Controller_t *)(can_instance->id))->dm_imfo_instance.Torque = torque;
    ((DM_J4310_Controller_t *)(can_instance->id))->dm_imfo_instance.T_Mos = t_mos;
    ((DM_J4310_Controller_t *)(can_instance->id))->dm_imfo_instance.T_Rotor = t_rotor;
}

DM_J4310_Controller_t *DM_4310_Register(CAN_HandleTypeDef *hcan, uint32_t protocol_id, uint32_t mst_id, uint16_t w_mode)
{
    DM_J4310_Controller_t *DM_J4310_s = (DM_J4310_Controller_t *)malloc(sizeof(DM_J4310_Controller_t));
    memset(DM_J4310_s, 0, sizeof(DM_J4310_Controller_t));
    DM_J4310_s->mode = w_mode;
    DM_J4310_s->protocol_id = protocol_id;

    if (w_mode == mit_mode)
        DM_J4310_s->mode_trans_id = protocol_id + 0x000;
    else if (w_mode == pos_vel_mode)
        DM_J4310_s->mode_trans_id = protocol_id + 0x100;
    else if (w_mode == vel_mode)
        DM_J4310_s->mode_trans_id = protocol_id + 0x200;

    CAN_Init_Config_s can_instance_config = {0};
    can_instance_config.can_handle = hcan;
    can_instance_config.tx_id = DM_J4310_s->mode_trans_id;
    can_instance_config.rx_id = mst_id;             // 返回帧id
    can_instance_config.can_module_callback = NULL; // 这里可以设置回调函数,但是目前没有用到
    can_instance_config.id = DM_J4310_s;

    DM_J4310_s->can_instance = CANRegister(&can_instance_config); // 注册CAN实例

    DM_J4310_instnce[dm_idx++] = DM_J4310_s;

    return DM_J4310_s; // 返回电机实例指针
}

void DM_Change_Mode(DM_J4310_Controller_t *dm_j4310_instance, uint8_t to_mode)
{
    dm_j4310_instance->mode = to_mode; // 设置新的工作模式
    if (to_mode == mit_mode)
    {
        dm_j4310_instance->mode_trans_id = dm_j4310_instance->protocol_id + 0x000;
    }
    else if (to_mode == pos_vel_mode)
    {
        dm_j4310_instance->mode_trans_id = dm_j4310_instance->protocol_id + 0x100;
    }
    else if (to_mode == vel_mode)
    {
        dm_j4310_instance->mode_trans_id = dm_j4310_instance->protocol_id + 0x200;
    }
    dm_j4310_instance->can_instance->tx_id = dm_j4310_instance->mode_trans_id;
}
void Enable_DM(DM_J4310_Controller_t *dm_j4310_instance)
{
    uint8_t motor_data[8] = {0}; // 发送数据缓存
    motor_data[0] = 0xFF;
    motor_data[1] = 0xFF;
    motor_data[2] = 0xFF;
    motor_data[3] = 0xFF;                                            // 使能电机
    motor_data[4] = 0xFF;                                            // 使能电机
    motor_data[5] = 0xFF;                                            // 使能电机
    motor_data[6] = 0xFF;                                            // 使能电机
    motor_data[7] = 0xFC;                                            // 使能电机
    memcpy(dm_j4310_instance->can_instance->tx_buff, motor_data, 8); // 将数据拷贝到CAN实例的发送缓存中
    CANTransmit(dm_j4310_instance->can_instance);                    // 发送数据
}
void Control_DM(DM_J4310_Controller_t *dm_j4310_instance)
{

//    float P_MAX, V_MAX, T_MAX;
//    P_MAX = 12.5f;
//    V_MAX = 30.f;
//    T_MAX = 10.f;

    uint16_t Postion_Tmp, Velocity_Tmp, Torque_Tmp, KP_Tmp, KD_Tmp;
    uint8_t motor_data[8] = {0}; // 发送数据缓存

    if (dm_j4310_instance->mode == mit_mode)
    {

        // 1. 编码位置（16位有符号，范围由调试助手设定）
        int16_t p_des_raw = FloatToI16(dm_j4310_instance->dm_controller_instance.P_des, -30.0f, 30.0f, 16); // 示例：假设位置范围±π rad
        motor_data[0] = (p_des_raw >> 8) & 0xFF;                                                            // D1: p_des[15:8]
        motor_data[1] = p_des_raw & 0xFF;                                                                   // D2: p_des[7:0]

        // 2. 编码速度（12位有符号，范围由调试助手设定）
        int16_t v_des_raw = FloatToI16(dm_j4310_instance->dm_controller_instance.V_des, -30.0f, 30.0f, 12); // 示例：速度范围±30 rad/s
        motor_data[2] = (v_des_raw >> 4) & 0xFF;                                                            // D3: v_des[11:4]
        uint8_t v_des_low = (v_des_raw & 0x0F) << 4;                                                        // D4: v_des[3:0]（占高4位）

        // 3. 编码Kp（12位无符号，范围0-500）
        uint16_t Kp_raw = FloatToU16(dm_j4310_instance->dm_controller_instance.Kp, 0.0f, 500.0f, 12);
        motor_data[3] = v_des_low | ((Kp_raw >> 8) & 0x0F); // D4: v_des[3:0] | Kp[11:8]
        motor_data[4] = Kp_raw & 0xFF;                      // D5: Kp[7:0]

        // 4. 编码Kd（12位无符号，范围0-5）
        uint16_t Kd_raw = FloatToU16(dm_j4310_instance->dm_controller_instance.Kd, 0.0f, 5.0f, 12);
        motor_data[5] = (Kd_raw >> 4) & 0xFF;  // D6: Kd[11:4]
        uint8_t Kd_low = (Kd_raw & 0x0F) << 4; // D7: Kd[3:0]（占高4位）

        // 5. 编码t_ff（12位有符号，范围-7~7 N·m）
        int16_t t_ff_raw = FloatToI16(dm_j4310_instance->dm_controller_instance.Torque, -7.0f, 7.0f, 12);
        motor_data[6] = Kd_low | ((t_ff_raw >> 8) & 0x0F); // D7: Kd[3:0] | t_ff[11:8]
        motor_data[7] = t_ff_raw & 0xFF;                   // D8: t_ff[7:0]
    }
    else if (dm_j4310_instance->mode == pos_vel_mode)
    {
        memcpy(&motor_data[0], (uint8_t *)&dm_j4310_instance->dm_controller_instance.P_des, 4);
        memcpy(&motor_data[4], (uint8_t *)&dm_j4310_instance->dm_controller_instance.V_des, 4);
    }
    else if (dm_j4310_instance->mode == vel_mode)
    {
        memcpy(&motor_data[0], (uint8_t *)&dm_j4310_instance->dm_controller_instance.V_des, 4);
    }

    memcpy(dm_j4310_instance->can_instance->tx_buff, motor_data, 8); // 将数据拷贝到CAN实例的发送缓存中
    CANTransmit(dm_j4310_instance->can_instance);                    // 发送数据
}