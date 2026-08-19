/*
 * Oled_Task.c - OLED 显示任务（1.3寸 SH1106，软件I2C：PB10=SCL/PB9=SDA，地址0x78）
 *
 * 职责：
 *   OLED 硬件初始化（OLED_Init，含上电延时与初始化序列）在 main 初始化阶段完成；
 *   本任务只负责画面绘制与周期刷新，把耗时刷屏从主初始化路径中剥离出来。
 *
 * 显示内容：
 *   第1行：HelloWorld（静态）
 *   第2行：OLED Test OK（静态）
 *   第3行：Run: 运行秒数（每秒刷新，验证任务存活与持续刷屏正常）
 */
#include "Oled_Task.h"

#include "FreeRTOS.h"
#include "task.h"
#include "OLED.h"
#include "bsp_log.h"

/* ---- 任务参数（集中配置） ---- */
#define OLED_TASK_PRIORITY      4U    /* 低于 LedTask(5)，高于日志任务(1) */
#define OLED_TASK_STACK_WORDS   256U
#define OLED_REFRESH_PERIOD_MS  1000U /* 第三行秒数刷新周期 */

static TaskHandle_t s_oled_task;

static void oled_task(void *arg)
{
    (void)arg;

    LOG_I("OledTask", "启动：首帧绘制 HelloWorld / OLED Test OK");

    /* 首帧静态画面（一次绘制，后续仅刷新秒数行） */
    OLED_Clear();
    OLED_ShowString(0, 0,  "HelloWorld",  OLED_8X16);
    OLED_ShowString(0, 16, "OLED Test OK", OLED_8X16);
    OLED_Update();

    uint32_t seconds = 0U;
    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(OLED_REFRESH_PERIOD_MS));
        seconds++;

        /* 第三行显示运行秒数：既证明任务存活，也持续验证刷屏链路 */
        OLED_ShowString(0, 32, "Run:", OLED_8X16);
        OLED_ShowNum(40, 32, seconds, 6, OLED_8X16);
        OLED_Update();
    }
}

void Oled_Task_Init(void)
{
    xTaskCreate(oled_task, "OledTask", OLED_TASK_STACK_WORDS, NULL,
                OLED_TASK_PRIORITY, &s_oled_task);
}