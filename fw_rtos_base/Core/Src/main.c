/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2025 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "cmsis_os.h"
#include "can.h"
#include "dma.h"
#include "tim.h"
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "dm_j4310.h"
#include "maincpp.h"
#include "math.h"
#include "Led_Task.h"
#include "bsp_usart.h"
#include "OLED.h"
#include "Oled_Task.h"
#include "SdCard_Task.h"
#include "version.h"
#include "bsp_log.h"
#include "sys_status.h"
#include "sd_cli.h"
extern void UART10_Init(void);
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define fireVelo_low 5
#define fireVelo_high 9.5
// 高精度二次模型（误差最小）
int calculate_rpm(float desired_velocity) {
    const float A =3745, B = -3339.759f;
    return (int)( A * desired_velocity +B);
}
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */
	float debug_shoot_velocity=0;
int get_rpm=0;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
void MX_FREERTOS_Init(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_CAN2_Init();
  MX_USART3_UART_Init();
  MX_USART6_UART_Init();
  MX_TIM4_Init();
  MX_TIM5_Init();
  MX_CAN1_Init();
  /* USER CODE BEGIN 2 */
  /* ---- 阶段1：最小系统验证（流水灯） ----
   * 暂时停用：舵机/遥控任务（main_cpp）、单电机测试（MotorTest）。
   * 只保留：POWER 供电控制（基础设施）+ 日志串口 + LED 流水灯。
   */
  HAL_GPIO_WritePin(POWER1_CTRL_GPIO_Port,POWER1_CTRL_Pin,1);
  HAL_GPIO_WritePin(POWER2_CTRL_GPIO_Port,POWER2_CTRL_Pin,1);
  HAL_GPIO_WritePin(POWER3_CTRL_GPIO_Port,POWER3_CTRL_Pin,1);
  HAL_GPIO_WritePin(POWER4_CTRL_GPIO_Port,POWER4_CTRL_Pin,0);

  // main_cpp();          /* 阶段1停用：舵机+遥控任务 */
  // MotorTest_Task_Init();/* 阶段1停用：单电机测试 */
  UART10_Init();            /* 日志串口必须先于 LED 任务（任务里 printf 依赖 uart10） */
  SD_CLI_Init();             /* SD 卡命令行（uart10 接收回调注册） */
  LOG_I("fw", "=== firmware %s (release %s, built %s %s) ===",
        FW_VERSION_STR, FW_RELEASE_STR, FW_BUILD_DATE, FW_BUILD_TIME);  /* 双版本标识 */
  Led_Task_Init();          /* LED 流水灯任务 */
  OLED_Init();              /* OLED 显示屏（软件I2C：PB10=SCL/PB9=SDA，地址0x78） */
  Oled_Task_Init();      /* 状态屏任务（二分法排查结束，恢复） */
  SdCard_Task_Init();
  xTaskCreate(SD_CLI_TaskEntry, "CliTask", 512, NULL, 3, NULL);  /* SD 命令行任务 */
  SYS_SetState(SYS_STATE_RUNNING);         /* OLED 显示任务：画面绘制/周期刷新由任务执行 */
//	HAL_TIM_PWM_Start(&htim4,TIM_CHANNEL_1);
//	int debug_pwm=0;

		
//	__HAL_TIM_SET_COMPARE(&htim4,TIM_CHANNEL_1,500);
	// main_cpp();  /* 阶段1停用：舵机+遥控任务（CubeMX 重新生成时自动还原，已重新注释） */




//__HAL_TIM_SET_COMPARE(&htim4,TIM_CHANNEL_1,500);
//HAL_Delay(200);
//__HAL_TIM_SET_COMPARE(&htim4,TIM_CHANNEL_2,500);
	
//	uint8_t data[16]={0};
//	 HAL_UART_Receive_DMA(&huart6,(uint8_t*)data,sizeof(data));
//bsp_can_1_config();
//HAL_Delay(1);   
// DM_4310_Register(&hcan1,0x001,0x000,mit_mode);

//	DM_J4310_instnce[0]->dm_controller_instance.p_des=12;
//DM_J4310_instnce[0]->dm_controller_instance.Kp = 1;
//DM_J4310_instnce[0]->dm_controller_instance.Kd =1;

// Enable_DM(DM_J4310_instnce[0]);
//bsp_can_2_config();
//HAL_Delay(3);   
//达妙j4310初始id为0x01
//dm_motor_init();
//HAL_Delay(1);
//motor[Motor1].ctrl.mode=mit_mode;
//  motor[Motor1].id=0x01;
// dm_motor_enable(&hcan2, &motor[Motor1]);
// HAL_Delay(1000) ;
// mit_ctrl(&hcan2, &motor[Motor1], motor[Motor1].id, 10, 50, 0, 5, 0);
//uint32_t jntm = 0;


//MI_Motor_s xm_1;
//// MI_motor_GetID(&xm_1);
//// HAL_Delay(3);                                                                                                    ·	 
//// xm_1.motor_id = 0X7F;
//xm_1.motor_id = 0X7F;
//xm_1.phcan = &hcan2;
//MI_motor_Enable(&xm_1);
//HAL_Delay(3);
//MI_motor_SpeedControl(&xm_1, 5, 0.3);
// HAL_Delay(3);
// HAL_Delay(3000);

//   MI_motor_ChangeID(&xm_1, 0x7F, 0x01);
//   HAL_Delay(3);
//     xm_1.motor_id = 0X01;
//     MI_motor_SpeedControl(&xm_1, 5, 0.3);
//   MI_motor_Stop(&xm_1);
// HAL_Delay(3000);

//  MI_motor_SpeedControl(&xm_1, 25, 0.3);
// HAL_Delay(3);
   // // 开启dm电机
//	 DM_J4310_Controller_t *Rise_DMj4310;
//  Rise_DMj4310 = DM_4310_Register(&hcan1, 0x02, 0x03, pos_vel_mode);
//  Enable_DM(Rise_DMj4310);
//  HAL_Delay(10);
//  Rise_DMj4310->dm_controller_instance.P_des = 0;
//  Rise_DMj4310->dm_controller_instance.V_des = 3;


  /* USER CODE END 2 */

  /* Call init function for freertos objects (in cmsis_os2.c) */
  MX_FREERTOS_Init();

  /* Start scheduler */
  osKernelStart();

  /* We should never get here as control is now taken by the scheduler */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {                                                     
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
//		get_rpm= calculate_rpm(debug_shoot_velocity);
//		
//		       Control_DM(Rise_DMj4310);
// 
////	 HAL_UART_Receive_DMA(&huart6,(uint8_t*)data,sizeof(data));
//		HAL_Delay(6);
//			__HAL_TIM_SET_COMPARE(&htim4,TIM_CHANNEL_1,debug_pwm);
//		HAL_Delay(10);
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 6;
  RCC_OscInitStruct.PLL.PLLN = 168;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 7;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
