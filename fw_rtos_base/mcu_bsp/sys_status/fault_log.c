/*
 * fault_log.c - 崩溃黑匣子实现（RTC 备份寄存器 + 开机自报）
 * 设计说明见 fault_log.h
 */
#include "fault_log.h"

#include "main.h"    /* RTC / SCB / HAL_PWR_* */
#include "bsp_log.h"

#define FAULTLOG_MAGIC (0x5A5A0000u) /* 高 24 位魔数（区分"有效记录"与"随机值"），低 8 位 = kind */

void FaultLog_Store(uint32_t kind, uint32_t a, uint32_t b, uint32_t c)
{
    RTC->BKP0R = FAULTLOG_MAGIC | (kind & 0xFFu);
    RTC->BKP1R = a;
    RTC->BKP2R = b;
    RTC->BKP3R = c;
}

void FaultLog_StoreStackOverflow(const char *task)
{
    uint32_t name = 0u;
    uint8_t i;

    if (task != NULL)
    {
        for (i = 0u; i < 4u; i++)
        {
            name = (name << 8) | (uint32_t)(uint8_t)task[i];
            if (task[i] == '\0')
            {
                break;
            }
        }
    }
    FaultLog_Store(FAULTLOG_KIND_STACKOVF, name, (uint32_t)SCB->CFSR, 0u);
}

void FaultLog_InitAndReport(void)
{
    uint32_t b0;
    uint32_t b1;
    uint32_t b2;
    uint32_t b3;

    /* 开备份域写权限（此后崩溃路径才能写 BKP 寄存器）。PWR 时钟必须先开。 */
    __HAL_RCC_PWR_CLK_ENABLE();
    HAL_PWR_EnableBkUpAccess();

    b0 = RTC->BKP0R;
    b1 = RTC->BKP1R;
    b2 = RTC->BKP2R;
    b3 = RTC->BKP3R;

    if ((b0 & 0xFFFFFF00u) != FAULTLOG_MAGIC)
    {
        LOG_I("fault", "black box: clean (no crash recorded since power-on)");
        return;
    }

    if ((b0 & 0xFFu) == FAULTLOG_KIND_HARDFAULT)
    {
        LOG_E("fault", "PREVIOUS RUN CRASHED: HardFault survived reset -> PC=0x%08lX LR=0x%08lX CFSR=0x%08lX (0x%08lX)",
              (unsigned long)b1, (unsigned long)b2, (unsigned long)b3, (unsigned long)0u);
    }
    else if ((b0 & 0xFFu) == FAULTLOG_KIND_STACKOVF)
    {
        char nm[5];

        nm[0] = (char)((b1 >> 24) & 0xFFu);
        nm[1] = (char)((b1 >> 16) & 0xFFu);
        nm[2] = (char)((b1 >> 8) & 0xFFu);
        nm[3] = (char)(b1 & 0xFFu);
        nm[4] = '\0';
        LOG_E("fault", "PREVIOUS RUN CRASHED: STACK OVERFLOW in task '%s' -> raise that task's stack (CFSR=0x%08lX)",
              nm, (unsigned long)b2);
    }
    else
    {
        LOG_E("fault", "PREVIOUS RUN CRASHED: unknown kind 0x%02lX (a=0x%08lX b=0x%08lX c=0x%08lX)",
              (unsigned long)(b0 & 0xFFu), (unsigned long)b1, (unsigned long)b2, (unsigned long)b3);
    }

    RTC->BKP0R = 0u; /* 清标记：保证下次崩溃能重新记录 */
}
