/*
 * proto_tx.h - 协议统一发送口（跨任务串行化）
 *
 * 为什么需要：协议回包（CmdRx）、异步事件（J8108 控制任务）、周期遥测（Monitor）来自**三个任务**，
 *           都写同一个 USART6。若不加锁，两行会交错成"半行 + 半行"，上位机解析器会吃到脏行。
 * 做法：一把互斥锁 + 单次 blocking 发送整行（行内自带 \r\n），锁粒度 = 一行。
 *
 * 已知边界：bsp_log 的日志输出**不走本锁**（改它要动公共基础设施）。因此：
 *   · 协议行本身是原子的（不会被另一条协议行打断）；
 *   · 极端情况下一条日志可能插进协议行中间 → 上位机需按"只信 @ 开头且完整的行"处理；
 *   · 联调时用 `#LOG 0` 把日志压到只剩错误，即可基本消除该风险（协议 §5.1）。
 */
#ifndef PROTO_TX_H
#define PROTO_TX_H

#include "stdint.h"

void Proto_TxInit(void);             /* 创建互斥锁（调度器启动前调用亦可） */
void Proto_Send(const char *line);   /* 加锁 → 发送 line + "\r\n" → 解锁（line 为 NUL 结尾，≤127 字节） */
uint32_t Proto_TxLines(void);        /* 累计发出的协议行数（遥测/诊断用） */
uint32_t Proto_TxBusyDrops(void);    /* 取 TX 锁超时被丢弃的行数（>0 说明有写入方被卡住过） */

#endif /* PROTO_TX_H */
