#ifndef __CYBERGEAR_H__
#define __CYBERGEAR_H__ 

#include "main.h"
#include "bsp_can.h"

extern uint8_t MI_MASTERID ;
#define P_MIN -12.5f
#define P_MAX 12.5f
#define V_MIN -30.0f
#define V_MAX 30.0f
#define KP_MIN 0.0f
#define KP_MAX 500.0f
#define KD_MIN 0.0f
#define KD_MAX 5.0f
#define T_MIN -12.0f
#define T_MAX 12.0f

#define IQ_REF_MIN -27.0f
#define IQ_REF_MAX 27.0f
#define SPD_REF_MIN -30.0f
#define SPD_REF_MAX 30.0f
#define LIMIT_TORQUE_MIN 0.0f
#define LIMIT_TORQUE_MAX 12.0f
#define CUR_FILT_GAIN_MIN 0.0f
#define CUR_FILT_GAIN_MAX 1.0f
#define LIMIT_SPD_MIN 0.0f
#define LIMIT_SPD_MAX 30.0f
#define LIMIT_CUR_MIN 0.0f
#define LIMIT_CUR_MAX 27.0f

typedef enum
{
    OK                 = 0,//无故障
    BAT_LOW_ERR        = 1,//欠压故障
    OVER_CURRENT_ERR   = 2,//过流
    OVER_TEMP_ERR      = 3,//过温
    MAGNETIC_ERR       = 4,//磁编码故障
    HALL_ERR_ERR       = 5,//HALL编码故障
    NO_CALIBRATION_ERR = 6//未标定
}motor_state_e;//电机状态（故障信息）

typedef enum
{
    CONTROL_MODE  = 0, //运控模式
    LOCATION_MODE = 1, //位置模式
    SPEED_MODE    = 2, //速度模式
    CURRENT_MODE  = 3  //电流模式
} motor_run_mode_e;//电机运行模式

typedef enum
{
    IQ_REF        = 0X7006,//电流模式Iq指令
    SPD_REF       = 0X700A,//转速模式转速指令
    LIMIT_TORQUE  = 0X700B,//转矩限制
    CUR_KP        = 0X7010,//电流的 Kp 
    CUR_KI        = 0X7011,//电流的 Ki 
    CUR_FILT_GAIN = 0X7014,//电流滤波系数filt_gain
    LOC_REF       = 0X7016,//位置模式角度指令
    LIMIT_SPD     = 0X7017,//位置模式速度设置
    LIMIT_CUR     = 0X7018 //速度位置模式电流设置
} motor_index_e;//电机功能码

typedef enum
{
    RESET_MODE = 0,//Reset模式[复位]
    CALI_MODE  = 1,//Cali 模式[标定]
    RUN_MODE   = 2 //Motor模式[运行]
} motor_mode_state_e;//电机模式状态

typedef struct
{
    uint32_t motor_id : 8; // 只占8位
    uint32_t data : 16;
    uint32_t mode : 5;
    uint32_t res : 3;
} __attribute__((packed)) EXT_ID_t; // 32位扩展ID解析结构体

typedef struct
{
    uint32_t info : 24;
    uint32_t communication_type : 5;
    uint32_t res : 3;
}__attribute__((packed)) RxCAN_info_s;// 解码内容缓存

typedef struct
{
    uint32_t FE : 8;
    uint32_t motor_id : 16;
    uint32_t communication_type : 5;
    uint32_t res : 3;
    uint32_t MCU_id;
}__attribute__((packed)) RxCAN_info_type_0_s;// 通信类型0解码内容

typedef struct
{
    uint32_t master_can_id : 8;
    uint32_t motor_id : 8;
    uint32_t under_voltage_fault : 1;
    uint32_t over_current_fault : 1;
    uint32_t over_temperature_fault : 1;
    uint32_t magnetic_encoding_fault : 1;
    uint32_t HALL_encoding_failure : 1;
    uint32_t unmarked : 1;
    uint32_t mode_state : 2;
    uint32_t communication_type : 5;
    uint32_t res : 3;
    float angle;//(rad)
    float speed;//(rad/s)
    float torque;//(N*m)
    float temperature;//(℃)
} __attribute__((packed)) RxCAN_info_type_2_s; // 通信类型2解码内容

typedef struct
{
    uint32_t motor_id : 8;
    uint32_t master_can_id : 16;
    uint32_t communication_type : 5;
    uint32_t res : 3;
    uint16_t index;
    float param;
    
}__attribute__((packed)) RxCAN_info_type_17_s;// 通信类型17解码内容

typedef struct
{
    CAN_HandleTypeDef *phcan;
    motor_state_e motor_state;
    motor_mode_state_e  motor_mode_state;
    EXT_ID_t EXT_ID;
    uint8_t motor_id;
    uint8_t txdata[8];
    RxCAN_info_type_2_s RxCAN_info;
} MI_Motor_s;


uint32_t FloatToUint(float x, float x_min, float x_max, int bits);
void XM_motor_CanTx(MI_Motor_s* hmotor);
float RangeRestrict(float x, float x_min, float x_max)  ;
void MI_motor_GetID(MI_Motor_s* hmotor) ;
void MI_motor_ChangeID(MI_Motor_s* hmotor,uint8_t Now_ID,uint8_t Target_ID);
void MI_motor_Enable(MI_Motor_s* hmotor)   ;
void MI_motor_Stop(MI_Motor_s* hmotor);
void MI_motor_Control(MI_Motor_s* hmotor, float torque, float MechPosition , float speed , float kp , float kd);
void MI_motor_TorqueControl(MI_Motor_s* hmotor, float torque);
void MI_motor_LocationControl(MI_Motor_s* hmotor, float location, float kp, float kd);
void MI_motor_SpeedControl(MI_Motor_s* hmotor, float speed, float kd);






#endif 
