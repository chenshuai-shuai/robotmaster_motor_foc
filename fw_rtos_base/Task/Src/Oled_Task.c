/**
 * @file    Oled_Task.c
 * @brief   OLED 系统状态屏（SH1106 软件I2C：PB10=SCL/PB9=SDA）
 *
 * 显示布局（200ms 刷新）：
 *   第1行：固件版本 v0.1.0-dev
 *   第2行：SYS: <系统状态>（BOOT/INIT/RUNNING/SD WAIT/...）
 *   第3行：SD : <SD 子状态> + 格式化进度（块计数）
 *   第4行：运行时间 uptime（秒）
 *
 * 旧的 HelloWorld/Test OK/Run 计数显示已删除——OLED 进入实用阶段，
 * 现在它是系统状态指示器：配合 LED + 日志，卡死时一眼定位最后状态。
 */
#include "Oled_Task.h"

#include "FreeRTOS.h"
#include "task.h"
#include "OLED.h"
#include "bsp_log.h"
#include "version.h"
#include "sys_status.h"

extern volatile int g_rw_result;   /* -100 未测 / 0 PASS / 其他失败 */

/* ---- 任务参数（集中配置） ---- */
#define OLED_TASK_PRIORITY      4U    /* 低于 LedTask(5)，高于日志任务(1) */
#define OLED_TASK_STACK_WORDS 512U
#define OLED_REFRESH_PERIOD_MS  200U  /* 状态屏刷新周期 */

/* 格式化块进度（sd_diskio.c 写计数，OLED 显示进度用） */
extern uint32_t sd_diskio_get_write_count(void);

static TaskHandle_t s_oled_task;

static void oled_task(void *arg)
{
    (void)arg;

    uint32_t seconds = 0U;
    uint32_t last_tick = 0U;

    LOG_I("OledTask", "start: system status screen");

    for (;;)
    {
        uint32_t tick = xTaskGetTickCount();

        OLED_Clear();
        /* 首行：发布版 | 测试版（防新旧固件混淆） */
        OLED_ShowString(0, 0,  FW_RELEASE_STR, OLED_8X16);             /* 发布版 */
        OLED_ShowString(48, 0, "|", OLED_8X16);
        OLED_ShowString(56, 0, FW_VERSION_STR, OLED_8X16);             /* 测试版 */
        OLED_ShowString(0, 16, "SYS:", OLED_8X16);
        OLED_ShowString(32, 16, SYS_StateText(SYS_GetState()), OLED_8X16);

        /* 格式化状态带进度（块计数） */
        if (SYS_GetState() == SYS_STATE_SD_FORMAT)
        {
            OLED_ShowString(0, 32, "SD :FORMAT", OLED_8X16);
            OLED_ShowNum(72, 32, sd_diskio_get_write_count() / 1000UL, 4, OLED_8X16);
            OLED_ShowString(104, 32, "K", OLED_8X16);   /* 千块 */
        }
        else if (g_rw_result == 0)
        {
            OLED_ShowString(0, 32, "SD :RW PASS", OLED_8X16);   /* 读写校验通过 */
        }
        else if (g_rw_result > 0)
        {
            OLED_ShowString(0, 32, "SD :RW FAIL", OLED_8X16);   /* 读写失败 */
        }
        else
        {
            OLED_ShowString(0, 32, "SD :", OLED_8X16);
            OLED_ShowString(32, 32, SYS_StateText(SYS_GetState()), OLED_8X16);
        }

        /* 第 4 行：运行秒数 */
        if ((tick - last_tick) >= pdMS_TO_TICKS(1000U))
        {
            last_tick = tick;
            seconds++;
        }
        OLED_ShowString(0, 48, "UP:", OLED_8X16);
        OLED_ShowNum(32, 48, seconds, 6, OLED_8X16);

        OLED_Update();
        vTaskDelay(pdMS_TO_TICKS(OLED_REFRESH_PERIOD_MS));
    }
}

void Oled_Task_Init(void)
{
    xTaskCreate(oled_task, "OledTask", OLED_TASK_STACK_WORDS, NULL,
                OLED_TASK_PRIORITY, &s_oled_task);
}
