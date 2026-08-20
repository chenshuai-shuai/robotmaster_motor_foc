/*
 * Led_Task.c - 状态指示灯 + 跑马灯任务
 *
 * 灯组（对照原理图）：
 *   1) 板载 LED：绿 = PF14，红 = PE11，【低电平点亮】—— 系统状态指示：
 *      绿：心跳（频率随状态变化）  红：FAULT 常亮
 *      BOOT/INIT/RUNNING : 绿 1Hz 慢闪（系统活着）
 *      SD_WAIT          : 绿 0.5Hz 极慢闪（等待插卡）
 *      SD_INIT/MOUNT/FORMAT : 绿 4Hz 快闪（SD 忙，格式化时勿断电）
 *      SD_READY         : 绿常亮（一切就绪）
 *      FAULT            : 绿灭 + 红常亮
 *   2) 外接 8×LED：PG1 ~ PG8 跑马灯（接线验证/装饰，高电平点亮）
 * 引脚初始化自包含（不依赖 CubeMX 生成代码）。
 */
#include "Led_Task.h"

#include "FreeRTOS.h"
#include "task.h"
#include "bsp_log.h"
#include "main.h"
#include "sys_status.h"

/* ---- 板载 LED：低电平点亮 ---- */
#define LED_GREEN_PORT GPIOF
#define LED_GREEN_PIN  GPIO_PIN_14
#define LED_RED_PORT   GPIOE
#define LED_RED_PIN    GPIO_PIN_11

#define LED_ON(pin)   HAL_GPIO_WritePin(pin##_PORT, pin##_PIN, GPIO_PIN_RESET)
#define LED_OFF(pin)  HAL_GPIO_WritePin(pin##_PORT, pin##_PIN, GPIO_PIN_SET)

/* ---- 外接 8×LED：PG1~PG8 ---- */
#define LED_EXT_PORT      GPIOG
#define LED_EXT_PIN_MASK  (GPIO_PIN_1|GPIO_PIN_2|GPIO_PIN_3|GPIO_PIN_4|\
                           GPIO_PIN_5|GPIO_PIN_6|GPIO_PIN_7|GPIO_PIN_8)
#define LED_EXT_ON_LEVEL  GPIO_PIN_RESET
#define LED_EXT_ON(i)     HAL_GPIO_WritePin(LED_EXT_PORT, (GPIO_PIN_1 << (i)), LED_EXT_ON_LEVEL)
#define LED_EXT_OFF(i)    HAL_GPIO_WritePin(LED_EXT_PORT, (GPIO_PIN_1 << (i)), \
                                            (LED_EXT_ON_LEVEL == GPIO_PIN_SET) ? GPIO_PIN_RESET : GPIO_PIN_SET)

#define LED_TICK_MS   50U   /* LED 任务刷新粒度 */

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

/* 状态 → 绿/红 LED 行为 */
static void status_led_update(uint32_t tick)
{
    uint8_t state = SYS_GetState();
    uint32_t t = tick / LED_TICK_MS;   /* 50ms 单位 */

    switch (state)
    {
        case SYS_STATE_BOOT:
        case SYS_STATE_INIT:
        case SYS_STATE_RUNNING:
            LED_OFF(LED_RED);
            /* 绿 1Hz：亮 500ms 灭 500ms */
            HAL_GPIO_WritePin(LED_GREEN_PORT, LED_GREEN_PIN,
                              ((t / 10U) & 1U) ? GPIO_PIN_RESET : GPIO_PIN_SET);
            break;

        case SYS_STATE_SD_WAIT:
            LED_OFF(LED_RED);
            /* 绿 0.5Hz 极慢闪：等插卡 */
            HAL_GPIO_WritePin(LED_GREEN_PORT, LED_GREEN_PIN,
                              ((t / 20U) & 1U) ? GPIO_PIN_RESET : GPIO_PIN_SET);
            break;

        case SYS_STATE_SD_INIT:
        case SYS_STATE_SD_MOUNT:
        case SYS_STATE_SD_FORMAT:
            LED_OFF(LED_RED);
            /* 绿 4Hz 快闪：SD 忙（格式化中勿断电） */
            HAL_GPIO_WritePin(LED_GREEN_PORT, LED_GREEN_PIN,
                              ((t / 3U) & 1U) ? GPIO_PIN_RESET : GPIO_PIN_SET);
            break;

        case SYS_STATE_SD_READY:
            LED_OFF(LED_RED);
            LED_ON(LED_GREEN);   /* 绿常亮：一切就绪 */
            break;

        case SYS_STATE_FAULT:
        default:
            LED_OFF(LED_GREEN);
            LED_ON(LED_RED);     /* 红常亮：异常 */
            break;
    }
}

static void led_task(void *arg)
{
    (void)arg;

    uint32_t ext_step = 0U;

    for (;;)
    {
        uint32_t tick = xTaskGetTickCount();

        /* 板载双灯：系统状态指示 */
        status_led_update(tick);

        /* 外接 8 灯：跑马灯（每 150ms 一步，独立于状态指示） */
        if ((tick % pdMS_TO_TICKS(150U)) < pdMS_TO_TICKS(LED_TICK_MS))
        {
            LED_EXT_OFF((ext_step + 7U) % 8U);
            LED_EXT_ON(ext_step % 8U);
            ext_step++;
        }

        vTaskDelay(pdMS_TO_TICKS(LED_TICK_MS));
    }
}

void Led_Task_Init(void)
{
    led_gpio_init();
    xTaskCreate(led_task, "LedTask", 512, NULL, 5, &s_led_task);
}
