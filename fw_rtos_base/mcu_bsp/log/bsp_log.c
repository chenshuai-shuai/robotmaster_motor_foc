/*
 * @Author: skybase
 * @Date: 2025-01-13 17:12:42
 * @LastEditors: skybase
 * @LastEditTime: 2025-02-06 18:44:55
 * @Description:  ᕕ(◠ڼ◠)ᕗ​ 
 * @FilePath: \stm32H7_RT_THREAD\BSP\log\bsp_log.c
 */
#include "bsp_log.h"
#include <stdarg.h>
/* USER CODE BEGIN  */
#include "bsp_usart.h"
#include "usart.h"

#include "FreeRTOS.h"
#include "task.h"

extern USARTInstance uart10;

/* USER CODE END  */

/* ---- 内部状态 ----
 * 发送环形缓冲：生产者 debug_transmit（任务上下文），消费者日志任务。
 * 索引为 uint32 配合"容量-1"掩码回绕（缓冲容量为 2 的幂，见 bsp_log.h 参数区）。
 * 所有共享变量用 volatile + 临界区保护。 */

/* 发送通道（UART 主输出，USART6） */
static uint8_t s_log_tx_buf[LOG_BUF_SIZE];
static volatile uint32_t s_log_tx_wr = 0U;       /* 生产者写索引（临界区保护） */
static volatile uint32_t s_log_tx_rd = 0U;       /* 消费者读索引（仅日志任务修改） */
static volatile uint32_t s_log_tx_drop_cnt = 0U; /* 缓冲满时丢弃的整条日志数 */

static TaskHandle_t s_log_task_handle = NULL;    /* 日志任务句柄 */
static volatile uint8_t s_log_task_created = 0U; /* 任务创建标志（防重复创建） */

static const char s_level_char[5U] = { ' ', 'E', 'W', 'I', 'D' }; /* 级别前缀字符 */


static void log_tx_task(void *arg);   /* 日志发送任务：阻塞等通知，唤醒后消费缓冲 */
static void log_tx_flush(void);       /* 消费发送缓冲：USB 整包优先，UART 中断发送兜底 */

/* 向栈上格式化缓冲追加内容：自动防越界，返回新的写入位置 */
static int log_append(char *buf, int pos, const char *fmt, ...);


/* 上下文自适应临界区：
 * 统一使用 BASEPRI 保存/恢复（taskENTER_CRITICAL_FROM_ISR 系列），在任务、
 * USB 中断、FreeRTOS 调度器启动前三种上下文中都安全，且不触碰 uxCriticalNesting：
 *   调度器启动前：任务级 taskENTER_CRITICAL 因 uxCriticalNesting 初值为
 *   0xaaaaaaaa，退出时永远不会恢复 BASEPRI，导致 SysTick 被屏蔽、后续
 *   HAL_Delay 永久卡死（OLED 上电延时曾因此整板冻结）；
 *   中断上下文：任务级 API 首层进入会 configASSERT 失败（关中断死循环）。
 * BASEPRI 保存/恢复两者皆无问题；返回值交给成对的 log_exit_critical() 恢复。 */
static inline UBaseType_t log_enter_critical(void)
{
    return taskENTER_CRITICAL_FROM_ISR();
}

static inline void log_exit_critical(UBaseType_t saved)
{
    taskEXIT_CRITICAL_FROM_ISR(saved);
}


/**
 * @brief 初始化日志发送任务（幂等）
 *
 * 创建最低优先级任务专门消费发送缓冲：printf/log_out 调用者只写缓冲、不等待硬件。
 * bsp_log 内部在首次 debug_transmit 时自动调用一次；应用层也可提前显式调用。
 */
void bsp_log_init(void)
{
    UBaseType_t saved = log_enter_critical();
    if (s_log_task_created == 0U)
    {
        if (xTaskCreate(log_tx_task, "LogTx", LOG_TASK_STACK_WORDS, NULL,
                        LOG_TASK_PRIORITY, &s_log_task_handle) == pdPASS)
        {
            s_log_task_created = 1U;
        }
    }
    log_exit_critical(saved);
}


/**
 * @brief 统一日志出口：栈上格式化 + 一次提交（一行只产生一次任务通知）
 *
 * 与旧版全局 _debug 缓冲相比：消除多任务并发竞争；调用者开销 =
 * snprintf（栈上）+ 一次 memcpy + 一次通知，微秒级。
 * 自动补 CRLF，调用方无需写 \n；超长自动截断并以 "..." 提示。
 * 前缀格式：[tag] L: （时间戳/颜色由 LOG_ENABLE_* 开关控制）。
 *
 * @param level 日志级别（LOG_LEVEL_*）
 * @param tag   模块名（可为 NULL，输出 "-"）
 * @param fmt   格式化串
 */
void log_out(uint8_t level, const char *tag, const char *fmt, ...)
{
    if ((fmt == NULL) || (level > LOG_LEVEL_DEBUG) || (level == LOG_LEVEL_NONE))
    {
        return;
    }

    char buf[LOG_FMT_BUF_SIZE];
    int pos = 0;

#if (LOG_ENABLE_COLOR == 1U)
    static const char *const s_level_color[5U] =
    {
        "",            /* NONE */
        "\033[1;31m",  /* ERROR 红 */
        "\033[1;33m",  /* WARN 黄 */
        "\033[1;32m",  /* INFO 绿 */
        "\033[1;36m",  /* DEBUG 青 */
    };
    pos = log_append(buf, pos, "%s", s_level_color[level]);
#endif

#if (LOG_ENABLE_TIMESTAMP == 1U)
    pos = log_append(buf, pos, "[%08lu] ", (unsigned long)HAL_GetTick());
#endif

    pos = log_append(buf, pos, "[%s] %c: ", (tag != NULL) ? tag : "-", s_level_char[level]);

    /* 用户内容：剩余空间不足时 vsnprintf 安全截断 */
    va_list args;
    va_start(args, fmt);
    int r = (pos < (int)sizeof(buf)) ? vsnprintf(buf + pos, sizeof(buf) - (size_t)pos, fmt, args) : 0;
    va_end(args);
    if (r > 0)
    {
        pos += r;
    }

    /* 收尾：超出缓冲时以 "..." 提示并截断，统一补 CRLF */
    if (pos >= (int)sizeof(buf) - 5)
    {
        memcpy(&buf[sizeof(buf) - 5U], "...\r\n", 5U);
        pos = (int)sizeof(buf);
    }
    else
    {
        memcpy(&buf[pos], "\r\n", 2U);
        pos += 2;
    }

    debug_transmit((uint8_t *)buf, (uint16_t)pos);
}


/**
 * @brief 非阻塞日志发送（缓冲化，log_out 与 printf/fputc 的底层出口）
 *
 * 数据整条 memcpy 进发送环形缓冲（微秒级，不等待任何硬件），随后用任务通知
 * 唤醒日志任务消费；缓冲满时整条丢弃并计数，保证调用者永不阻塞。
 *
 * @param data 待发送数据
 * @param len  字节数
 */
void debug_transmit(uint8_t *data, uint16_t len)
{
    if ((data == NULL) || (len == 0U))
    {
        return;
    }

    /* 惰性创建日志任务：调度器启动前也允许创建（任务只入就绪表，不切换上下文），
     * 保证启动前积压的日志在调度器运行后由日志任务的首轮冲刷发出去。 */
    if (s_log_task_created == 0U)
    {
        bsp_log_init();
    }

    UBaseType_t saved = log_enter_critical();
    uint32_t avail = (s_log_tx_wr - s_log_tx_rd) & (LOG_BUF_SIZE - 1U);
    uint32_t free = LOG_BUF_SIZE - 1U - avail;
    if ((uint32_t)len > free)
    {
        /* 缓冲满：丢弃本次新日志并计数（最旧的启动日志保留，USB 补发时头部完整） */
        s_log_tx_drop_cnt++;
        log_exit_critical(saved);
        return;
    }

    /* 环形写入（处理跨回绕，分两段 memcpy） */
    uint32_t cont = LOG_BUF_SIZE - s_log_tx_wr;
    if ((uint32_t)len <= cont)
    {
        memcpy(&s_log_tx_buf[s_log_tx_wr], data, len);
    }
    else
    {
        memcpy(&s_log_tx_buf[s_log_tx_wr], data, cont);
        memcpy(&s_log_tx_buf[0], data + cont, (uint32_t)len - cont);
    }
    s_log_tx_wr = (s_log_tx_wr + len) & (LOG_BUF_SIZE - 1U);
    log_exit_critical(saved);

    /* 唤醒日志任务消费：调度器启动前不发通知（避免启动前调用任务级 FreeRTOS
     * API），积压数据由日志任务启动后的首轮冲刷兜底。 */
    if ((s_log_task_handle != NULL) &&
        (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED))
    {
        xTaskNotifyGive(s_log_task_handle);
    }
}


/**
 * @brief 获取发送缓冲满丢弃的日志条数
 * @return 丢弃条数
 */
uint32_t bsp_log_get_drop_count(void)
{
    UBaseType_t saved = log_enter_critical();
    uint32_t cnt = s_log_tx_drop_cnt;
    log_exit_critical(saved);
    return cnt;
}


/**
 * @brief 日志发送任务：阻塞等待通知，唤醒后消费发送缓冲
 *
 * 空闲时挂起在 ulTaskNotifyTake（不占 CPU）；数据到达（debug_transmit）
 * 或 USB 发送完成（完成回调）时被唤醒。
 */
static void log_tx_task(void *arg)
{
    (void)arg;

    /* 启动后先冲刷调度器启动前积压的日志（此时尚无任务通知） */
    log_tx_flush();
    for (;;)
    {
        log_tx_flush();
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }
}


/**
 * @brief 消费发送缓冲（仅日志任务调用）
 *
 * 通道策略（UART 唯一日志口，USART6 中断发送）：
 *   1) USART6 就绪：USARTSend(IT) 整段（<=LOG_UART_CHUNK_SIZE）提交，
 *      等硬件发完（TC 中断恢复 gState）再前进读索引；等待带 1s 超时保护，
 *      超时强制 abort 防止中断异常时任务空转；
 *   2) USART6 不可用（未初始化）：丢弃最旧一段并计数，防止任务空转。
 */
static void log_tx_flush(void)
{
    UBaseType_t saved;
    for (;;)
    {
        saved = log_enter_critical();
        uint32_t rd = s_log_tx_rd;
        uint32_t wr = s_log_tx_wr;
        log_exit_critical(saved);
        uint32_t avail = (wr - rd) & (LOG_BUF_SIZE - 1U);
        if (avail == 0U)
        {
            return; /* 缓冲已空，挂起等下一次通知 */
        }

        uint32_t chunk = (avail > LOG_UART_CHUNK_SIZE) ? LOG_UART_CHUNK_SIZE : avail;
        uint32_t cont = LOG_BUF_SIZE - rd; /* 到缓冲末尾的连续字节数 */
        if (chunk > cont)
        {
            chunk = cont;
        }

        /* ---- UART 主通道（USART6 中断发送，非阻塞） ---- */
        if ((uart10.usart_handle != NULL) &&
            (uart10.usart_handle->gState == HAL_UART_STATE_READY))
        {
            USARTSend(&uart10, &s_log_tx_buf[rd], (uint16_t)chunk, USART_TRANSFER_IT);

            /* 等硬件发送完成（TC 中断里 HAL 恢复 gState=READY），期间让出 CPU。
             * 发送期间生产者的空闲区计算基于未前进的读索引，不会覆盖在发数据。 */
            uint32_t timeout = 0U;
            while (uart10.usart_handle->gState != HAL_UART_STATE_READY)
            {
                if (++timeout > 1000U)
                {
                    /* 1s 超时：UART 中断异常，强制中止恢复通道 */
                    HAL_UART_AbortTransmit(uart10.usart_handle);
                    break;
                }
                vTaskDelay(1);
            }
            saved = log_enter_critical();
            s_log_tx_rd = (rd + chunk) & (LOG_BUF_SIZE - 1U);
            log_exit_critical(saved);
        }
        else
        {
            /* UART 不可用：丢弃该段并计数，防止任务空转 */
            saved = log_enter_critical();
            s_log_tx_rd = (rd + chunk) & (LOG_BUF_SIZE - 1U);
            s_log_tx_drop_cnt++;
            log_exit_critical(saved);
        }
    }
}


/**
 * @brief 向栈上格式化缓冲追加内容：自动防越界，返回新的写入位置
 *
 * @param buf 目标缓冲
 * @param pos 当前写入位置（0..LOG_FMT_BUF_SIZE）
 * @param fmt 格式化串
 * @return    新的写入位置（超界时钳制为 LOG_FMT_BUF_SIZE，后续调用安全短路）
 */
static int log_append(char *buf, int pos, const char *fmt, ...)
{
    if (pos >= (int)LOG_FMT_BUF_SIZE)
    {
        return (int)LOG_FMT_BUF_SIZE;
    }

    va_list args;
    va_start(args, fmt);
    int r = vsnprintf(buf + pos, (size_t)(LOG_FMT_BUF_SIZE - pos), fmt, args);
    va_end(args);
    return (r > 0) ? (pos + r) : pos;
}


/* printf 重定向（Retarget）：
 * 行攒发：字符先进小行缓冲，遇 '\n'（或缓冲满）才一次性提交，
 * 把"每字符一次任务通知"降为"每行一次"，日志量大时开销降一个数量级。
 * 注意：printf 并发调用时行内容可能交错（字节级串行，不破坏内存）；
 * 要求严格的模块请改用 LOG_E/W/I/D（log_out 一次提交，无交错）。 */
static char s_fputc_line[LOG_FPUT_BUF_SIZE];
static uint16_t s_fputc_len = 0U;
static const uint8_t s_crlf[2U] = { '\r', '\n' };

#include <stdio.h>
int fputc(int ch, FILE *f)
{
    (void)f;
    uint8_t c = (uint8_t)ch;

    if (c == (uint8_t)'\r')
    {
        return ch; /* 孤立 CR 忽略，统一由 '\n' 提交 CRLF */
    }

    UBaseType_t saved = log_enter_critical();
    if (c == (uint8_t)'\n')
    {
        /* 一行结束：提交行内容 + CRLF */
        if (s_fputc_len > 0U)
        {
            debug_transmit((uint8_t *)s_fputc_line, s_fputc_len);
            s_fputc_len = 0U;
        }
        debug_transmit((uint8_t *)s_crlf, 2U);
    }
    else if (s_fputc_len >= (LOG_FPUT_BUF_SIZE - 1U))
    {
        /* 超长无换行：先提交满行，当前字符另起一行 */
        debug_transmit((uint8_t *)s_fputc_line, s_fputc_len);
        s_fputc_line[0] = (char)c;
        s_fputc_len = 1U;
    }
    else
    {
        s_fputc_line[s_fputc_len++] = (char)c;
    }
    log_exit_critical(saved);

    return ch;
}
