/*
 * Disp_Task.c - 显示自检任务：屏的"上电稳定 + 软复位/唤醒 + 字库探测 + 自检/彩色体检"
 *
 * 为什么独立成任务（2026-09-19 实机教训，代价是好几轮"开机几秒什么都没有"的困惑）：
 *   这些步骤合计 0.4~4s（点灯期更长）。它们原来写在 main() 的 Disp_Init() 里 →
 *   **调度器启动被整个卡住**：期间日志任务（负责冲刷开机积压日志）与流水灯任务都还没被调度
 *   → 现象 = "开机几秒全静默，然后日志和灯同时突然开始"（用户第一眼会当成死机/初始化慢）。
 *   现在：main() 只配引脚/SPI（毫秒级）→ 立刻起调度器 → 日志/灯/UI 马上跑；
 *   屏在这个低优先级任务里慢慢初始化，完成前页面层的绘制自动空操作
 *   （驱动内部按 s_ready 门控，**不需要额外同步原语**）。
 *
 * 铁律：main()/Disp_Init() 里不许再出现 >10ms 的忙等（门禁有断言）——
 *   长延时/自检属于任务；调度器起来之前，任何长等待都在偷走"启动可观测性"。
 */
#include "feature_config.h"

#if (FEATURE_DISP_ST7735S_SPI || FEATURE_DISP_SH1106_I2C)

#include "FreeRTOS.h"
#include "task.h"
#include "Disp_Task.h"
#include "disp_port.h"
#include "bsp_log.h"

static TaskHandle_t s_disp_task;

static void disp_bringup_task(void *arg)
{
    (void)arg;

    Disp_BringUp(); /* 屏重型初始化 + 自检（单色驱动是空实现；内部按 ready 门控绘制） */

    LOG_I("disp", "bringup task done: stack headroom %u words free of %u",
          (unsigned)uxTaskGetStackHighWaterMark(NULL), (unsigned)DISP_TASK_STACK_WORDS);

    vTaskDelete(NULL); /* 一次性任务：做完即退（不再占调度器名额与栈） */
}

void Disp_Task_Init(void)
{
    if (xTaskCreate(disp_bringup_task, "DispTask", DISP_TASK_STACK_WORDS, NULL, DISP_TASK_PRIO,
                    &s_disp_task) != pdPASS)
    {
        LOG_E("disp", "xTaskCreate FAILED (heap/stack?) -> screen bringup skipped (draw = no-op)");
    }
}

#endif /* 两个显示驱动宏之一 */
