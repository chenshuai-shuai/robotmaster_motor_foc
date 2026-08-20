/**
 * @file    sys_status.c
 * @brief   系统状态实现（临界区保护 + 状态迁移日志）
 */
#include "sys_status.h"
#include "bsp_log.h"
#include "FreeRTOS.h"
#include "task.h"

static volatile uint8_t s_sys_state = SYS_STATE_BOOT;

/* SD 读写测试结果：-100=未测 0=PASS -1=内容不符 >0=FRESULT 错误码（OLED 显示用） */
volatile int g_rw_result = -100;

static const char *const s_state_text[] =
{
    [SYS_STATE_BOOT]     = "BOOT",
    [SYS_STATE_INIT]     = "INIT",
    [SYS_STATE_RUNNING]  = "RUNNING",
    [SYS_STATE_SD_WAIT]  = "WAIT CARD",
    [SYS_STATE_SD_INIT]  = "SD INIT",
    [SYS_STATE_SD_MOUNT] = "SD MOUNT",
    [SYS_STATE_SD_FORMAT]= "SD FORMAT",
    [SYS_STATE_SD_READY] = "SD READY",
    [SYS_STATE_FAULT]    = "FAULT",
};

void SYS_SetState(uint8_t state)
{
    if (state > SYS_STATE_FAULT)
    {
        return;
    }

    taskENTER_CRITICAL();
    uint8_t old = s_sys_state;
    s_sys_state = state;
    taskEXIT_CRITICAL();

    if (old != state)
    {
        LOG_I("sys", "state %s -> %s", SYS_StateText(old), SYS_StateText(state));
    }
}

uint8_t SYS_GetState(void)
{
    taskENTER_CRITICAL();
    uint8_t s = s_sys_state;
    taskEXIT_CRITICAL();
    return s;
}

const char *SYS_StateText(uint8_t state)
{
    if (state > SYS_STATE_FAULT)
    {
        return "?";
    }
    return s_state_text[state];
}
