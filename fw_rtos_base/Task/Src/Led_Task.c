/*
 * Led_Task.c - 流水灯任务（最小系统验证）
 *
 * 灯组（对照原理图）：
 *   1) 板载 LED：绿 = PF14，红 = PE11，【低电平点亮】（手册 1.5 节）
 *   2) 外接 8×LED：PG1 ~ PG8（GPIOG PIN1~8，用户自定义接口引出），【高电平点亮】
 *      （若你的 LED 接法相反，改 LED_EXT_ON_LEVEL 宏即可）
 *
 * 流程：启动全亮 1s 验证 8 灯 → 跑马灯循环（150ms/步）+ 板载双灯交替 + 串口日志
 * 引脚初始化自包含（不依赖 CubeMX 生成代码，防重新生成被覆盖）。
 */
#include "Led_Task.h"

#include "FreeRTOS.h"
#include "task.h"
#include "bsp_log.h"
#include "main.h"

/* ---- 板载 LED：低电平点亮 ---- */
#define LED_GREEN_PORT GPIOF
#define LED_GREEN_PIN  GPIO_PIN_14
#define LED_RED_PORT   GPIOE
#define LED_RED_PIN    GPIO_PIN_11

#define LED_ON(pin)   HAL_GPIO_WritePin(pin##_PORT, pin##_PIN, GPIO_PIN_RESET)
#define LED_OFF(pin)  HAL_GPIO_WritePin(pin##_PORT, pin##_PIN, GPIO_PIN_SET)

/* ---- 外接 8×LED：PG1~PG8，高电平点亮 ---- */
#define LED_EXT_PORT      GPIOG
#define LED_EXT_PIN_MASK  (GPIO_PIN_1|GPIO_PIN_2|GPIO_PIN_3|GPIO_PIN_4|\
                           GPIO_PIN_5|GPIO_PIN_6|GPIO_PIN_7|GPIO_PIN_8)
#define LED_EXT_ON_LEVEL  GPIO_PIN_RESET  /* 低电平点亮（LED 上拉接法：阳极接3V3，IO拉低亮） */

#define LED_EXT_ON(i)     HAL_GPIO_WritePin(LED_EXT_PORT, (GPIO_PIN_1 << (i)), LED_EXT_ON_LEVEL)
#define LED_EXT_OFF(i)    HAL_GPIO_WritePin(LED_EXT_PORT, (GPIO_PIN_1 << (i)), \
                                            (LED_EXT_ON_LEVEL == GPIO_PIN_SET) ? GPIO_PIN_RESET : GPIO_PIN_SET)

static TaskHandle_t s_led_task;

static void led_gpio_init(void)
{
    __HAL_RCC_GPIOF_CLK_ENABLE();   /* PF14 绿 */
    __HAL_RCC_GPIOE_CLK_ENABLE();   /* PE11 红 */
    __HAL_RCC_GPIOG_CLK_ENABLE();   /* PG1~8 外接灯组 */

    GPIO_InitTypeDef gpio = {0};
    gpio.Mode  = GPIO_MODE_OUTPUT_PP;
    gpio.Pull  = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;

    gpio.Pin = LED_GREEN_PIN;
    HAL_GPIO_Init(LED_GREEN_PORT, &gpio);
    gpio.Pin = LED_RED_PIN;
    HAL_GPIO_Init(LED_RED_PORT, &gpio);
    gpio.Pin = LED_EXT_PIN_MASK;
    HAL_GPIO_Init(LED_EXT_PORT, &gpio);

    LED_OFF(LED_GREEN);
    LED_OFF(LED_RED);
    HAL_GPIO_WritePin(LED_EXT_PORT, LED_EXT_PIN_MASK,
                      (LED_EXT_ON_LEVEL == GPIO_PIN_SET) ? GPIO_PIN_RESET : GPIO_PIN_SET);
}

static void led_task(void *arg)
{
    (void)arg;
    int i;

    // LOG_I("LedTask", "启动：板载绿PF14/红PE11 + 外接8灯PG1~8");  /* LED日志已关 */

    /* 第0步：8 灯全亮 1s（验证接线，逐个在串口报数） */
    for (i = 0; i < 8; i++) LED_EXT_ON(i);
    // LOG_I("LedTask", "8灯全亮 1 秒——检查接线");  /* LED日志已关 */
    vTaskDelay(1000);
    for (i = 0; i < 8; i++) LED_EXT_OFF(i);
    vTaskDelay(300);

    /* 第1步：跑马灯循环 + 板载双灯交替 */
    while (1)
    {
        for (i = 0; i < 8; i++)
        {
            /* 上一个灭，当前亮（循环衔接） */
            LED_EXT_OFF((i + 7) % 8);
            LED_EXT_ON(i);

            /* 板载灯随步交替 */
            if (i & 1) { LED_ON(LED_RED);   LED_OFF(LED_GREEN); }
            else       { LED_ON(LED_GREEN); LED_OFF(LED_RED);   }

            // LOG_I("LedTask", "第%d灯亮 (PG%d)", i + 1, i + 1);  /* LED日志已关 */
            vTaskDelay(150);
        }
    }
}

void Led_Task_Init(void)
{
    led_gpio_init();
    xTaskCreate(led_task, "LedTask", 256, NULL, 5, &s_led_task);
}
