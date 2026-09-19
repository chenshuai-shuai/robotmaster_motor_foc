/*
 * stack_probe.h - 任务栈余量自报（一次性，开机 5s 后打一条）
 *
 * 为什么要有：STM32 没有 MMU，**栈溢出 = 静默踩内存/死机**，事后极难查
 *   （实机 2026-09-18 遇到"发完某条命令板子就没动静了"，回包还被切断在半句 —— 典型崩溃现场）。
 *   本探针把 uxTaskGetStackHighWaterMark（开机以来的**最小剩余**，单位 words）打出来，
 *   一眼就知道裕量够不够；再配合 configCHECK_FOR_STACK_OVERFLOW=2 的钩子（会记录任务名到
 *   s_overflow_task，下次开机由 main.c 的 report_previous_fault() 自报），形成
 *   「提前预警 + 出事可查」两道防线。
 *
 * 用法：在每个任务主循环里调用
 *     stack_probe_tick(now_ms(), "J8108", J8108_TASK_STACK_WORDS);
 * static inline + 函数内 static done → 每个 .c 各有一份标志，互不干扰。
 */
#ifndef STACK_PROBE_H
#define STACK_PROBE_H

#include "FreeRTOS.h"
#include "task.h"
#include "bsp_log.h"

static inline void stack_probe_tick(uint32_t t_ms, const char *tag, uint32_t stack_words)
{
    static uint8_t done = 0u;

    if ((done == 0u) && (t_ms > 5000u))
    {
        done = 1u;
        LOG_I(tag, "stack headroom: %u words free of %u (must stay well above 0)",
              (unsigned)uxTaskGetStackHighWaterMark(NULL), (unsigned)stack_words);
    }
}

#endif /* STACK_PROBE_H */
