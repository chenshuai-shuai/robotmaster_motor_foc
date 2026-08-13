#include "CyberGear.h"
uint8_t MI_MASTERID = 1; //master id 发送指令时EXTID的bit8:15,反馈的bit0:7

/**
  * @brief          float转int，数据打包用
  * @param[in]      x float数值
  * @param[in]      x_min float数值的最小值
  * @param[in]      x_max float数值的最大值
  * @param[in]      bits  int的数据位数
  * @retval         none
  */
 uint32_t FloatToUint(float x, float x_min, float x_max, int bits)
 {
     float span = x_max - x_min;
     float offset = x_min;
     if(x > x_max) x=x_max;
     else if(x < x_min) x=x_min;
     return (uint32_t) ((x-offset)*((float)((1<<bits)-1))/span);
 }
/**
  * @brief          小米电机CAN通信发送
  * @param[in]      hmotor 电机结构体
  * @retval         none
  */
 void XM_motor_CanTx(MI_Motor_s* hmotor)
 {
    CAN_TxHeaderTypeDef CAN_TxHeader_MI;
     CAN_TxHeader_MI.DLC = 8;
     CAN_TxHeader_MI.IDE = CAN_ID_EXT;
     CAN_TxHeader_MI.RTR = CAN_RTR_DATA;
     CAN_TxHeader_MI.ExtId = *((uint32_t*)&(hmotor->EXT_ID));
    //  /*检测可用的发送邮箱*/
    //  uint32_t free_TxMailbox = HAL_CAN_GetTxMailboxesFreeLevel(hmotor->phcan);//检测是否有空闲邮箱
    //  while (free_TxMailbox<3){//等待空闲邮箱数达到3
    //      free_TxMailbox = HAL_CAN_GetTxMailboxesFreeLevel(hmotor->phcan);
    //  }
    //  /* 将发送信息添加到发送邮箱中 */
     uint32_t mailbox;
     HAL_CAN_AddTxMessage(hmotor->phcan, &CAN_TxHeader_MI, hmotor->txdata, &mailbox);//将发送的数据添加到发送邮箱中
 }

/**
  * @brief          输入范围限制
  * @param[in]      x 输入数值
  * @param[in]      x_min 输入数值的最小值
  * @param[in]      x_max 输入数值的最大值
  * @retval         none
  */
 float RangeRestrict(float x, float x_min, float x_max)
 {
     float res;
     if(x > x_max) res=x_max;
     else if(x < x_min) res=x_min;
     else res = x;
     return res;
 }

/**
  * @brief          获取设备ID （通信类型0），需在电机使能前使用
  * @param[in]      hmotor 电机结构体
  * @retval         none
  */
 void MI_motor_GetID(MI_Motor_s* hmotor)
 {
     hmotor->EXT_ID.mode = 0;
     hmotor->EXT_ID.data = 0;
     hmotor->EXT_ID.motor_id = 0;
     hmotor->EXT_ID.res = 0;
  
     for(uint8_t i=0; i<8; i++)
     {
         hmotor->txdata[i]=0;
     }
     
     XM_motor_CanTx(hmotor);
 }

/**
  * @brief          设置电机CAN_ID（通信类型7）更改当前电机CAN_ID , 立即生效，需在电机使能前使用
  * @param[in]      hmotor 电机结构体
  * @param[in]      Now_ID 电机现在的ID
  * @param[in]      Target_ID 想要改成的电机ID
  * @retval         none
  */
 void MI_motor_ChangeID(MI_Motor_s* hmotor,uint8_t Now_ID,uint8_t Target_ID)
 {
     hmotor->motor_id = Now_ID;
     hmotor->EXT_ID.mode = 7;	
     hmotor->EXT_ID.motor_id = Now_ID;
     hmotor->EXT_ID.data = Target_ID << 8 | MI_MASTERID;
     hmotor->EXT_ID.res = 0;
     for(uint8_t i=0; i<8; i++)
     {
         hmotor->txdata[i]=0;
     }
 
     XM_motor_CanTx(hmotor);
 }

/**
  * @brief          小米电机使能（通信类型 3）
  * @param[in]      hmotor 电机结构体
  * @param[in]      id 电机id
  * @retval         none
  */
 void MI_motor_Enable(MI_Motor_s* hmotor)
 {
     hmotor->EXT_ID.mode = 3;
     hmotor->EXT_ID.motor_id = hmotor->motor_id;
     hmotor->EXT_ID.data = MI_MASTERID;
     hmotor->EXT_ID.res = 0;
     for(uint8_t i=0; i<8; i++)
     {
         hmotor->txdata[i]=0;
     }
 
     XM_motor_CanTx(hmotor);
 }



 /**
  * @brief          电机停止运行帧（通信类型4）
  * @param[in]      hmotor 电机结构体
  * @retval         none
  */
void MI_motor_Stop(MI_Motor_s* hmotor)
{
    hmotor->EXT_ID.mode = 4;
    hmotor->EXT_ID.motor_id = hmotor->motor_id;
    hmotor->EXT_ID.data = MI_MASTERID;
    hmotor->EXT_ID.res = 0;
 
    for(uint8_t i=0; i<8; i++)
    {
        hmotor->txdata[i]=0;
    }

    XM_motor_CanTx(hmotor);
}

/**
  * @brief          运控模式电机控制指令（通信类型1）
  * @param[in]      hmotor 电机结构体
  * @param[in]      torque 目标力矩
  * @param[in]      MechPosition 
  * @param[in]      speed 
  * @param[in]      kp 
  * @param[in]      kd 
  * @retval         none
  */
 void MI_motor_Control(MI_Motor_s* hmotor, float torque, float MechPosition , float speed , float kp , float kd)
 {
     hmotor->EXT_ID.mode = 1;
     hmotor->EXT_ID.motor_id = hmotor->motor_id;
     hmotor->EXT_ID.data = FloatToUint(torque,T_MIN,T_MAX,16);
     hmotor->EXT_ID.res = 0;
  
     hmotor->txdata[0]=FloatToUint(MechPosition,P_MIN,P_MAX,16)>>8;
     hmotor->txdata[1]=FloatToUint(MechPosition,P_MIN,P_MAX,16);
     hmotor->txdata[2]=FloatToUint(speed,V_MIN,V_MAX,16)>>8;
     hmotor->txdata[3]=FloatToUint(speed,V_MIN,V_MAX,16);
     hmotor->txdata[4]=FloatToUint(kp,KP_MIN,KP_MAX,16)>>8;
     hmotor->txdata[5]=FloatToUint(kp,KP_MIN,KP_MAX,16);
     hmotor->txdata[6]=FloatToUint(kd,KD_MIN,KD_MAX,16)>>8;
     hmotor->txdata[7]=FloatToUint(kd,KD_MIN,KD_MAX,16);
 
     XM_motor_CanTx(hmotor);
 }


 /**
  * @brief          小米电机力矩控制模式控制指令
  * @param[in]      hmotor 电机结构体
  * @param[in]      torque 目标力矩
  * @retval         none
  */
void MI_motor_TorqueControl(MI_Motor_s* hmotor, float torque)
{
    MI_motor_Control(hmotor, torque, 0, 0, 0, 0);
}

/**
  * @brief          小米电机位置模式控制指令
  * @param[in]      hmotor 电机结构体
  * @param[in]      location 控制位置 rad
  * @param[in]      kp 响应速度(到达位置快慢)，一般取1-10
  * @param[in]      kd 电机阻尼，过小会震荡，过大电机会震动明显。一般取0.5左右
  * @retval         none
  */
void MI_motor_LocationControl(MI_Motor_s* hmotor, float location, float kp, float kd)
{
    MI_motor_Control(hmotor, 0, location, 0, kp, kd);
}

/**
  * @brief          小米电机速度模式控制指令
  * @param[in]      hmotor 电机结构体
  * @param[in]      speed 控制速度
  * @param[in]      kd 响应速度，一般取0.1-1
  * @retval         none
  */
void MI_motor_SpeedControl(MI_Motor_s* hmotor, float speed, float kd)
{
    MI_motor_Control(hmotor, 0, 0, speed, 0, kd);
} 