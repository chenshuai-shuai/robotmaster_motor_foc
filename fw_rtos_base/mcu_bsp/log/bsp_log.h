/*
 * @Author: skybase
 * @Date: 2025-01-13 17:12:42
 * @LastEditors: skybase
 * @LastEditTime: 2025-01-17 22:00:27
 * @Description:  ᕕ(◠ڼ◠)ᕗ​
 * @FilePath: \MDK-ARM\Code\bsp_log.h
 */
#ifndef _BSP_LOG_H_
#define _BSP_LOG_H_

#include <stdio.h>
#include <string.h>
#include "main.h"

/* ---- 日志级别（编译期裁剪，集中配置） ----
 * 调用级别低于 BSP_LOG_LEVEL 的日志在预处理阶段展开为空语句：
 * 字符串常量与格式化代码整体不进入固件（零 CPU、零 Flash 开销）。
 * 注意：被裁剪的日志参数表达式不会执行，勿在其中放有副作用的调用。 */
#define LOG_LEVEL_NONE  0U  /* 关闭全部日志 */
#define LOG_LEVEL_ERROR 1U
#define LOG_LEVEL_WARN  2U
#define LOG_LEVEL_INFO  3U
#define LOG_LEVEL_DEBUG 4U

#define BSP_LOG_LEVEL   LOG_LEVEL_DEBUG  /* 当前级别：发布/比赛固件建议 LOG_LEVEL_WARN */

/* ---- 日志通道参数（集中配置） ---- */
/* 发送环形缓冲：上电起全量缓存，USB 枚举成功后统一补发（USB 是唯一日志口）。
 * 4KB 足以容纳完整启动日志 + 枚举前数秒运行日志；缓冲满时丢弃新日志保头部。 */
#define LOG_BUF_SIZE         4096U   /* 发送环形缓冲总字节数（2 的幂） */
#define LOG_USB_CHUNK_SIZE   64U     /* USB FS IN 端点单包上限 */
#define LOG_RX_BUF_SIZE      256U    /* USB 接收环形缓冲字节数（2 的幂） */
#define LOG_TASK_PRIORITY    1U      /* 发送任务优先级：最低，让出 CPU */
#define LOG_TASK_STACK_WORDS 192U    /* 发送任务栈深度（字） */
#define LOG_FMT_BUF_SIZE     128U    /* 单条日志栈上格式化缓冲，超长截断 */
#define LOG_FPUT_BUF_SIZE    128U    /* fputc(printf) 行攒发缓冲 */

/* ---- 可选输出格式开关 ---- */
#define LOG_ENABLE_COLOR     0U      /* 1：ANSI 颜色前缀（每条多约 10 字节） */
#define LOG_ENABLE_TIMESTAMP 0U      /* 1：毫秒时间戳前缀（依赖 HAL_GetTick） */

/* ---- 统一日志宏（推荐使用，一次调用 = 一行日志） ----
 * tag 为模块名（如 "LedTask"/"can"），log_out 自动补 CRLF，调用方无需写 \n。 */
#if (BSP_LOG_LEVEL >= LOG_LEVEL_ERROR)
#define LOG_E(tag, ...) log_out(LOG_LEVEL_ERROR, tag, __VA_ARGS__)
#else
#define LOG_E(tag, ...) ((void)0)
#endif

#if (BSP_LOG_LEVEL >= LOG_LEVEL_WARN)
#define LOG_W(tag, ...) log_out(LOG_LEVEL_WARN, tag, __VA_ARGS__)
#else
#define LOG_W(tag, ...) ((void)0)
#endif

#if (BSP_LOG_LEVEL >= LOG_LEVEL_INFO)
#define LOG_I(tag, ...) log_out(LOG_LEVEL_INFO, tag, __VA_ARGS__)
#else
#define LOG_I(tag, ...) ((void)0)
#endif

#if (BSP_LOG_LEVEL >= LOG_LEVEL_DEBUG)
#define LOG_D(tag, ...) log_out(LOG_LEVEL_DEBUG, tag, __VA_ARGS__)
#else
#define LOG_D(tag, ...) ((void)0)
#endif

/* ---- 核心接口 ---- */

/* 栈上格式化一条日志并一次提交（自动补 CRLF；无共享缓冲竞争，多任务安全） */
void log_out(uint8_t level, const char *tag, const char *fmt, ...);

/* 非阻塞写发送缓冲（log_out 与 printf/fputc 的底层出口） */
void debug_transmit(uint8_t *data, uint16_t len);

/* 初始化：创建日志发送任务（幂等；bsp_log 内部首次打印时自动调用） */
void bsp_log_init(void);

/* 发送缓冲满时丢弃的日志条数（调试观测用） */
uint32_t bsp_log_get_drop_count(void);

/* 非阻塞读取 USB 接收环形缓冲：返回实际读出字节数，读出后即从缓冲移除 */
uint32_t bsp_log_rx_read(uint8_t *out, uint32_t maxlen);

/* ---- USB CDC 回调接入接口（仅 usbd_cdc_if.c 的 USER CODE 区调用） ---- */

/* CDC_Init_FS/CDC_DeInit_FS 维护 USB 枚举状态：置位后日志任务切 USB 通道，清零回落 UART */
void bsp_log_set_usb_connected(uint8_t connected);

/* CDC_TransmitCplt_FS（USB 中断上下文）调用：唤醒日志任务发送下一包 */
void bsp_log_notify_tx_cplt_from_isr(void);

/* CDC_Receive_FS（USB 中断上下文）调用：把主机发来的数据推入接收环形缓冲 */
void bsp_log_rx_push(const uint8_t *data, uint32_t len);

#endif