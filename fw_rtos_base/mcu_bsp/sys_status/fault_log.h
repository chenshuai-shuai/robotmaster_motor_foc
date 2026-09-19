/*
 * fault_log.h - 崩溃黑匣子（写进 RTC 备份寄存器，**复位后仍保留**，开机自报）
 *
 * 为什么：STM32 无 MMU，崩溃（HardFault / 栈溢出）在现场表现就是"板子突然没动静、
 *   回包还被切断在半句"，而 .bss 里的现场变量在**任何复位后都会被清零**（启动代码 ZI 清零），
 *   一旦用户按了复位/重新烧录就什么都查不到了。
 *   备份寄存器（RTC->BKPxR）位于备份域：只要 VDD/VBAT 在，**复位（含看门狗/软件复位）不清**，
 *   于是可以在崩溃瞬间写入、下次开机读出来打印 —— 免调试器拿到死因。
 *
 * 用法：
 *   1) 崩溃路径（HardFault_Handler / vApplicationStackOverflowHook）调用 FaultLog_Store*
 *      —— 只写寄存器，不做任何耗时/可能阻塞的事（崩溃上下文里最安全）
 *   2) 开机（main 里日志就绪后）调用 FaultLog_InitAndReport()：
 *      开备份域写权限 + 读上次死因并打印 + 清标记（保证下次崩溃能重新记录）
 *
 * 注意：**断电（拔电/掉电）会清**备份寄存器 → 崩溃后请按**复位**（不要断电），才能读到报告。
 */
#ifndef FAULT_LOG_H
#define FAULT_LOG_H

#include "stdint.h"

#define FAULTLOG_KIND_HARDFAULT (1u)
#define FAULTLOG_KIND_STACKOVF (2u)

/* 崩溃路径调用：kind + 三个 32 位现场值（只写寄存器，安全） */
void FaultLog_Store(uint32_t kind, uint32_t a, uint32_t b, uint32_t c);

/* 崩溃路径便捷封装：把任务名前 4 字符打包进 a，CFSR 放 b */
void FaultLog_StoreStackOverflow(const char *task);

/* 开机调用：开启备份域写权限 + 报告上次死因 + 清标记 */
void FaultLog_InitAndReport(void);

#endif /* FAULT_LOG_H */
