#include "maincpp.h"
#define PI 3.1415926535
#include "FreeRTOS.h"
#include "task.h"
#include "servo.h"
#include "control.h"
#include "tim.h"

/* ---- 小车项目：单电机测试任务（M2006+C610�?---- */
#include "MotorTest_Task.h"
#include "bsp_usart.h"
extern "C" void UART10_Init(void);

float x = 0.0f;
float z = 0.0f;
float yaw = 0.0f;
float debug_velocity= 300.0f;

#define QUEUE_LENGTH 10
#define ITEM_SIZE sizeof(float)



TaskHandle_t Omain_handle;
TaskHandle_t Servo1_Motor_handle;
TaskHandle_t Servo2_Motor_handle;
TaskHandle_t Remote_control_handle;

Servo::Servo_base_t servo_theta1;
Servo::Servo_base_t servo_theta2;
Servo::Servo_base_t servo_theta3;

Control::Control_t control;

void Onmain_Task(void *pvParameters);
void remoteControl(void *pvParameters);
void OnServo1_Control(void *pvParameters);
void OnServo2_Control(void *pvParameters);
// void message_update(void *pvParameters);
void main_cpp(void)
{
	
	//增大，舵机顺时针�?  servo_theta1 = Servo::Servo_base_t(&htim4, TIM_CHANNEL_1,reverse_dir,90, 0.0f, 180.0f,0.0f, 160.0f);//
  servo_theta2 = Servo::Servo_base_t(&htim4, TIM_CHANNEL_2,pos_dir,0, 0.0f, 180.0f, 0.0f, 140.0f); //
	  servo_theta3 = Servo::Servo_base_t(&htim4, TIM_CHANNEL_3,pos_dir,80, 0.0f, 180.0f, 0.0f, 150.0f); 
  servo_theta1._state=finished;
  servo_theta2._state=finished;
	 servo_theta3._state=finished;
  control = Control::Control_t(&servo_theta1, &servo_theta2);
	
  BaseType_t ok1 = xTaskCreate(Onmain_Task, "Onmain_Task", 600, NULL, 6, &Omain_handle);
  BaseType_t ok2 = xTaskCreate(OnServo1_Control, "Servo1_Motor", 1000, NULL, 3, &Servo1_Motor_handle);
  BaseType_t ok3 = xTaskCreate(OnServo2_Control, "Servo2_Motor", 1000, NULL, 3, &Servo2_Motor_handle);
  BaseType_t ok4 = xTaskCreate(remoteControl, "remote_contorl", 500, NULL, 5, &Remote_control_handle);

  if (ok2 != pdPASS || ok3 != pdPASS || ok4 != pdPASS)
  {
    while (1)
    {
    }
  }

  /* ---- 小车项目：日志串�?+ 单电机测试任�?----
   * 注意：先跑通单电机（开环电流斜坡），再扩展 4 电机�?   * CubeMX 需启用 USART6（或�?uart10_def.c 里的句柄）作为日志口�?   */
  UART10_Init();
  MotorTest_Task_Init();
}

void Onmain_Task(void *pvParameters)
{
  while (1)
  {
    vTaskDelay(50);
  }
}

void remoteControl(void *pvParameters)
{
  while (1)
  {
   if(servo_theta1._state==finished&&servo_theta2._state==finished)
    control.controlUpdate(x, z, yaw);

    vTaskDelay(20);
  }
}

void OnServo1_Control(void *pvParameters)
{

  while (1)
  {
    servo_theta1.control(debug_velocity);
    vTaskDelay(200);
  }
}

void OnServo2_Control(void *pvParameters)
{
  while (1)
  {
    servo_theta2.control(debug_velocity);
    vTaskDelay(200);
  }
}

extern "C"
{

#ifdef __MICROLIB
#include <stdio.h>

  int fputc(int ch, FILE *f)
  {
    (void)f;
    (void)ch;

    return ch;
  }
#else
#include <rt_sys.h>

  FILEHANDLE $Sub$$_sys_open(const char *name, int openmode)
  {
    (void)name;
    (void)openmode;
    return 0;
  }
#endif

  void _sys_exit(int ret)
  {
    (void)ret;
    while (1)
    {
    }
  }
  void _ttywrch(int ch)
  {
    (void)ch;
  }
}