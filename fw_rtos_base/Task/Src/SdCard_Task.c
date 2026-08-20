/**
 * @file    SdCard_Task.c
 * @brief   SD 卡检测与挂载任务（首个测试程序：识别 → 格式化/挂载 → 容量汇报）
 *
 * 状态机：
 *   NO_CARD    -- 200ms 轮询 PE15 检测插卡（日志提示当前检测电平，供实测定极性）
 *   CARD_IN    -- 初始化 SDIO+卡 → 挂载（无文件系统则自动格式化）→ 汇报容量
 *   READY      -- 已挂载；卡拔出 → 卸载 → 回 NO_CARD
 *
 * 参数集中配置（头部）。
 */
#include "SdCard_Task.h"

#include "FreeRTOS.h"
#include "task.h"
#include "bsp_log.h"
#include "sys_status.h"

extern volatile int g_rw_result;
#include "sd_sdio.h"
#include "sd_fs.h"

/* ---- 任务参数（集中配置） ---- */
#define SDCARD_TASK_PRIORITY    3U    /* 低于 Led(5)/Oled(4)，高于日志(1) */
#define SDCARD_TASK_STACK_WORDS 2048U /* f_mkfs+log_out(vsnprintf) 深链，512 曾栈溢出踩 TCB 致 PendSV HardFault */
#define SDCARD_POLL_MS          200U  /* 无卡时检测轮询周期 */

typedef enum
{
    SD_STATE_NO_CARD = 0,
    SD_STATE_CARD_IN,
    SD_STATE_READY,
} sd_state_e;

static TaskHandle_t s_sdcard_task;

static void sdcard_task(void *arg)
{
    (void)arg;

    sd_state_e state = SD_STATE_NO_CARD;
    uint32_t last_detect_report = 0U;
    uint32_t tick = 0U;

    SYS_SetState(SYS_STATE_SD_WAIT);
    LOG_I("SdCard", "start: waiting for card (PE15 detect, poll %dms)", SDCARD_POLL_MS);

    for (;;)
    {
        tick = xTaskGetTickCount();
        uint8_t inserted = SD_SDIO_CardInserted();

        switch (state)
        {
            case SD_STATE_NO_CARD:
            {
                if (inserted)
                {
                    LOG_I("SdCard", "card inserted, initializing SDIO...");
                    SYS_SetState(SYS_STATE_SD_INIT);
                    state = SD_STATE_CARD_IN;
                    break;
                }
                /* 每 5 秒汇报一次检测电平，供首次实测确定 SD_DETECT_INSERTED_LEVEL */
                if ((tick - last_detect_report) >= pdMS_TO_TICKS(5000U))
                {
                    last_detect_report = tick;
                    LOG_I("SdCard", "no card, PE15=%d (if this line never changes when inserting,"
                                    " flip SD_DETECT_INSERTED_LEVEL in sd_sdio.h)",
                          (int)HAL_GPIO_ReadPin(SD_DETECT_PORT, SD_DETECT_PIN));
                }
                break;
            }

            case SD_STATE_CARD_IN:
            {
                SD_SDIO_Init();
                SD_SDIO_DumpCardInfo();
                SYS_SetState(SYS_STATE_SD_MOUNT);
                FRESULT fr = SD_FS_Mount();
                if (fr != FR_OK)
                {
                    SYS_SetState(SYS_STATE_FAULT);
                    LOG_E("SdCard", "mount/formatted failed (FR=%d), retry in 2s", (int)fr);
                    vTaskDelay(pdMS_TO_TICKS(2000U));
                    break;
                }

                uint32_t total_mb = SD_FS_TotalMB();
                if (total_mb > 0U)
                {
                    LOG_I("SdCard", "READY: card mounted, capacity=%lu MB (%.1f GB)",
                          (unsigned long)total_mb, (double)total_mb / 1024.0);
                }
                else
                {
                    LOG_W("SdCard", "mounted but capacity query failed");
                }

                /* 读写任务：写 test.txt → 读回 → 校验（结果上 OLED + 日志） */
                SYS_SetState(SYS_STATE_SD_MOUNT);
                vTaskDelay(pdMS_TO_TICKS(500U));  /* 稳定期：让启动日志排空，写路径独占系统 */
                LOG_I("SdCard", "RW TEST: writing test.txt & verifying...");
                uint32_t rb = 0U;
                int rw = SD_FS_RWTest(&rb);
                g_rw_result = rw;   /* OLED 显示 */
                if (rw == 0)
                {
                    LOG_I("SdCard", "RW TEST PASS (%lu bytes verified)", (unsigned long)rb);
                }
                else
                {
                    LOG_E("SdCard", "RW TEST FAIL (ret=%d)", rw);
                }

                SYS_SetState(SYS_STATE_SD_READY);
                state = SD_STATE_READY;
                break;
            }

            case SD_STATE_READY:
            {
                if (!inserted)
                {
                    LOG_I("SdCard", "card removed, unmounting");
                    SD_FS_Unmount();
                    SYS_SetState(SYS_STATE_SD_WAIT);
                    state = SD_STATE_NO_CARD;
                }
                break;
            }

            default:
                state = SD_STATE_NO_CARD;
                break;
        }

        vTaskDelay(pdMS_TO_TICKS(SDCARD_POLL_MS));
    }
}

void SdCard_Task_Init(void)
{
    xTaskCreate(sdcard_task, "SdCard", SDCARD_TASK_STACK_WORDS, NULL,
                SDCARD_TASK_PRIORITY, &s_sdcard_task);
}
