/*
 * ui_status.h - UI/协议共享的"运行状态视图"（普通结构体，无动态内存）
 *
 * 数据来源：J8108 快照（帧一致） + J8108 控制状态（模式/设定点/心跳） + Monitor/协议自身计数。
 * 由 Monitor_Task 组装（唯一填充者），Oled 渲染器只读，Screen/协议都不用再去摸驱动内部结构。
 */
#ifndef UI_STATUS_H
#define UI_STATUS_H

#include "stdint.h"

#define UI_PAGE_COUNT (4u)
#define UI_LAST_CMD_LEN (14u)

/* 页面编号（与 docs/协议_串口控制_v1.md §13 对应） */
#define UI_PAGE_LINK (0u)   /* P0: CAN 四态 / rx,tx / 回传 Hz / ERR 位 */
#define UI_PAGE_MOTION (1u) /* P1: 模式 / 使能 / pos,vel,T / 设定点 / 温度 */
#define UI_PAGE_SERIAL (2u) /* P2: 协议态 / 最近命令 / 行计数 / 心跳剩余 / 最近错误 */
#define UI_PAGE_SYSTEM (3u) /* P3: 版本 / 运行时间 / free heap / 任务数 / 最近事件 */

typedef struct
{
    uint8_t page;     /* 当前页（0..UI_PAGE_COUNT-1） */
    uint8_t page_cnt; /* 页数 */

    /* 控制态（来自 J8108 控制任务） */
    uint8_t mode;        /* Ctrl_Mode_e */
    uint8_t enabled;     /* 1=电机已使能 */
    uint8_t frames_on;   /* 1=正在发控制帧（链路在跑） */
    uint8_t inpos;       /* 位置模式到位 */
    float set_deg;       /* 位置设定点 deg（POS 模式有效） */
    float set_dps;       /* 速度设定点 deg/s（SPEED 模式有效） */
    float set_nm;        /* 力矩设定点 N·m（TORQUE/IMP 有效） */
    float kp_now;        /* 本帧 Kp */
    float kd_now;        /* 本帧 Kd */
    uint16_t wd_left_ms; /* 心跳剩余时间（超时→阻尼） */
    uint32_t ev_flags;   /* 保护事件位（CTRL_EV_*） */
    uint16_t fb_hz;      /* 实测反馈帧率 Hz（1s 窗口） */
    uint8_t err_code;    /* 最近一次 @ERR 码（0=无） */

    /* 协议计数（CmdRx） */
    uint32_t rx_lines; /* 累计收到的命令行 */
    uint32_t tx_lines; /* 累计发出的 @ 行 */
    uint32_t rx_bytes; /* 累计收到的原始字节（诊断用） */
    char last_cmd[UI_LAST_CMD_LEN]; /* 最近一条命令名（大写，ASCII） */
    uint16_t tel_period_ms;         /* 遥测周期（0=关） */

    /* 系统（Monitor） */
    uint32_t uptime_s;
    uint32_t free_heap;
    uint32_t task_cnt;
} Ui_Status_t;

#endif /* UI_STATUS_H */
