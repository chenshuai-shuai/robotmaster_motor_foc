/*
 * proto_tx.c - 协议统一发送口实现（一把互斥锁 + 单次 blocking 发送整行）
 *
 * 用途见 proto_tx.h。发送走 bsp_usart 的 BLOCKING 模式（HAL_UART_Transmit 超时 100ms，
 * 对本工程最长 127 字节行 @115200 = 11ms 有充足余量）。
 */
#include "proto_tx.h"

#include "FreeRTOS.h"
#include "semphr.h"
#include "bsp_usart.h"
#include "bsp_log.h"

extern USARTInstance uart10; /* uart10_def.c（USART6 日志/协议同口） */

#define PROTO_LINE_MAX (127u)
#define PROTO_TX_WAIT_MS (50u) /* 取 TX 锁的最长等待（有界，防被永久占死） */

static SemaphoreHandle_t s_tx_mtx = NULL;
static char s_line[PROTO_LINE_MAX + 3u]; /* 含 \r\n\0 */
static uint32_t s_tx_lines = 0u;
static volatile uint32_t s_tx_busy_drops = 0u; /* 取锁超时被丢弃的行数 */

void Proto_TxInit(void)
{
    s_tx_mtx = xSemaphoreCreateMutex();
    if (s_tx_mtx == NULL)
    {
        LOG_E("proto", "TX mutex create FAILED -> protocol TX DISABLED (heap?)");
    }
    else
    {
        LOG_I("proto", "proto TX ready: one whole line per lock (replies @OK/@ERR/@EVT/@TEL)");
    }
}

void Proto_Send(const char *line)
{
    uint16_t n = 0u;
    uint16_t i;

    if ((line == NULL) || (s_tx_mtx == NULL))
    {
        return;
    }
    while ((line[n] != '\0') && (n < PROTO_LINE_MAX))
    {
        n++;
    }
    if (n == 0u)
    {
        return;
    }

    for (i = 0u; i < n; i++)
    {
        s_line[i] = line[i];
    }
    s_line[n] = '\r';
    s_line[n + 1u] = '\n';
    n = (uint16_t)(n + 2u);

    /* ★ 有界等待（不用 portMAX_DELAY）：万一某个任务在持锁期间崩了/挂了，
     *   协议口不会被永久锁死（否则所有 @ 回包静默 → 现场表现就是"板子卡死"） */
    if (xSemaphoreTake(s_tx_mtx, pdMS_TO_TICKS(PROTO_TX_WAIT_MS)) == pdTRUE)
    {
        (void)USARTSend(&uart10, (uint8_t *)s_line, n, USART_TRANSFER_BLOCKING);
        s_tx_lines++;
        xSemaphoreGive(s_tx_mtx);
    }
    else
    {
        s_tx_busy_drops++; /* 记一笔：主机可通过 @EVT/计数发现（不阻塞、不静默） */
    }
}

uint32_t Proto_TxLines(void)
{
    return s_tx_lines;
}

uint32_t Proto_TxBusyDrops(void)
{
    return s_tx_busy_drops;
}
