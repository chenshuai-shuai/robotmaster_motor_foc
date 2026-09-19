/*
 * motor_8108.h - 8108 关节电机 CAN 驱动（**双编版口径**）
 *
 * 协议依据：厂商《6010 关节电机用户文档（双编）V1.2》（2026-09-17 摄入知识库）
 *   用户 2026-09-17 确认：本机 8108 为**双编版** → 一律以双编文档为准。
 *
 *   - 命令帧  ID = 0x000 + CANID（默认 0x01）：数据 FF FF FF FF FF FF FF 码
 *             码：0xFC 使能 / 0xFD 失能 / 0xFE 位置零点 / 0xFB 重启 / 0xFA 清除错误位
 *   - MIT 帧  ID = 0x200 + CANID（0x201）：p_des16 | v_des12 | Kp12 | Kd12 | t_ff12（大端位拼装）
 *   - 反馈帧  ID = 0x780 + CANID（0x781）：byte0 ERR 报错位 / byte1-2 POS16 /
 *             byte3 VEL[11:4] / byte4 VEL[3:0]|T[11:8] / byte5 T[7:0] / byte6 TMos / byte7 TMotor
 *   - 量程（双编文档口径）：P ±12.56 rad（**输出端**，无需 ÷8）/ V ±45 rad/s（输出端）/
 *             Kp 0~500 / Kd 0~5 / T ±18 N·m
 *   - **通信方式：一发一收**（文档 §3.1 原文）—— 电机不回传就无法自发，必须持续发控制帧
 *   - byte0 报错位：bit7 过载 / bit6 线圈过温 / bit5 MOS 过温 / bit4 过流 / bit3 过压 / bit2 欠压
 *   注意：与 DJI 电调（0x200 命令帧体系）ID 不冲突；0x01 命令帧电调会忽略
 */
#ifndef MOTOR_8108_H
#define MOTOR_8108_H

#include "stdint.h"
#include "can.h"
#include "bsp_can.h"

/* ============================ 口径常量（全局唯一换算点） ============================ */
/* J8108_SCALE_OUTPUT_SIDE: 1 = 原始值即**输出端**（双编，文档口径，不除减速比）
 *                         0 = 原始值为电机端、输出端需 ÷GEAR（单编兜底，仅当实验 A 判为 Encoder:1） */
#define J8108_SCALE_OUTPUT_SIDE (1u)

#define J8108_GEAR_RATIO (8.0f)  /* 减速比 8:1（仅兜底口径使用） */
#define J8108_DIR (+1.0f)        /* 正方向符号（实验 B3：输出轴正转 pos 增大 → +1） */

#if J8108_SCALE_OUTPUT_SIDE
#define J8108_P_LO (-12.56f) /* 位置量程下限 rad（输出端；= 双编文档 PMAX 默认） */
#define J8108_P_HI (12.56f)
#else
#define J8108_P_LO (-25.12f) /* 单编口径（电机端） */
#define J8108_P_HI (25.12f)
#endif

#define J8108_V_LO (-45.0f) /* 速度量程 rad/s（输出端） */
#define J8108_V_HI (45.0f)
#define J8108_KP_LO (0.0f)
#define J8108_KP_HI (500.0f)
#define J8108_KD_LO (0.0f)
#define J8108_KD_HI (5.0f)
#define J8108_T_LO (-18.0f) /* 力矩量程 N·m（输出端） */
#define J8108_T_HI (18.0f)

#define J8108_RAD2DEG (57.29578f)
#define J8108_DEG2RAD (0.017453292f)

/* ---- 协议参数（改 CANID 需与电机参数区一致） ---- */
#define J8108_CAN_ID (0x01u)
#define J8108_CMD_ID (0x000u + J8108_CAN_ID)
#define J8108_MIT_ID (0x200u + J8108_CAN_ID)
#define J8108_FB_ID  (0x780u + J8108_CAN_ID)

/* ---- 命令码 ---- */
#define J8108_CMD_ENABLE (0xFCu)
#define J8108_CMD_DISABLE (0xFDu)
#define J8108_CMD_ZERO (0xFEu)
#define J8108_CMD_REBOOT (0xFBu)
#define J8108_CMD_CLEAR_ERR (0xFAu) /* 双编新增：清除错误位 */

/* ---- byte0 报错位（双编文档 §3.1） ---- */
#define J8108_ERR_OVERLOAD (0x80u)  /* bit7 过载 */
#define J8108_ERR_TP_COIL (0x40u)   /* bit6 电机线圈过温 */
#define J8108_ERR_TP_MOS (0x20u)    /* bit5 MOS 过温 */
#define J8108_ERR_OC (0x10u)        /* bit4 过流 */
#define J8108_ERR_OV (0x08u)        /* bit3 过压 */
#define J8108_ERR_UV (0x04u)        /* bit2 欠压 */

typedef struct
{
    uint8_t raw[8];      /* 最近一帧反馈原始数据 */
    uint32_t rx_count;   /* 累计收到反馈帧数（判链路通断） */
    uint32_t tx_cnt;     /* 累计发送帧数（诊断显示） */
    uint32_t last_rx_ms; /* 最近一帧反馈的接收时刻 ms（中断里用 FromISR 取时基） */
    uint8_t err;         /* byte0 报错位（0=无错） */
    float pos;           /* **输出端**位置 rad（双编口径，不除 8） */
    float vel;           /* **输出端**速度 rad/s */
    float torque;        /* 输出端力矩 N·m */
    float t_mos;         /* MOS 温度 C */
    float t_rotor;       /* 线圈温度 C */
} J8108_Feedback_t;

/* ---- CAN 链路状态（屏上可视化：初始化 / 通道就绪 / 总线故障）---- */
typedef enum
{
    J8108_ST_INIT_FAIL = 0u, /* CAN 注册或启动失败（外设未初始化 / 设备表满） */
    J8108_ST_INIT_OK = 1u,   /* CAN 已启动，总线上还没收到任何帧（等待节点） */
    J8108_ST_READY = 2u,     /* 收到反馈帧 → 通道已建立，随时可发数据 */
    J8108_ST_BUS_ERR = 3u    /* 总线故障（HAL 错误非 0：无应答 / 接线 / 终端电阻） */
} J8108_Status_e;

typedef struct
{
    CANInstance *can_instance;
    uint8_t init_ok;            /* CAN 注册+启动结果（1=成功） */
    uint8_t status;             /* J8108_Status_e */
    uint32_t bus_err;           /* 最近一次 HAL_CAN_GetError（0=正常） */
    uint32_t tx_dropped;        /* 因邮箱长时间无空位而丢弃的帧数（>0 说明总线卡过） */
    volatile uint8_t new_frame; /* 中断置位：有新反馈帧待解算（任务侧临界区取走） */
    J8108_Feedback_t fb;
} J8108_t;

/* ---- 数据快照：给显示/日志/协议用的"帧一致"只读视图（seqlock） ---- */
typedef struct
{
    uint32_t seq;      /* 序号（偶数=完整；发布时 +2） */
    uint8_t valid;     /* 1 = 至少收到过一帧反馈 */
    uint8_t status;    /* J8108_Status_e */
    uint8_t init_ok;
    uint8_t err;       /* byte0 报错位 */
    uint32_t bus_err;
    uint32_t rx_count;
    uint32_t tx_cnt;
    uint32_t frame_age_ms; /* 发布时刻 − 最近一帧时刻（读侧免算，判新鲜度） */
    uint8_t raw[8];        /* 与该帧解算值**同帧**的原始字节 */
    float pos;             /* 输出端 rad */
    float vel;             /* 输出端 rad/s */
    float torque;          /* 输出端 N·m */
    float pos_deg;         /* 输出端 deg（= pos × RAD2DEG，双编口径不除 8） */
    float vel_dps;         /* 输出端 deg/s */
    float vel_rpm;         /* 输出端 RPM */
    float t_mos;           /* MOS 温度 C */
    float t_rotor;         /* 线圈温度 C */
} J8108_Snapshot_t;

#ifdef __cplusplus
extern "C"
{
#endif

void J8108_Init(CAN_HandleTypeDef *hcan);
J8108_t *J8108_Get(void);
void J8108_SendCmd(uint8_t cmd_code);
/* 发 MIT 控制帧。**入参单位：输出端** rad / rad·s⁻¹ / N·m（与反馈同口径；DIR 在内部处理） */
void J8108_SendMIT(float p_rad, float v_rps, float kp, float kd, float t_nm);
void J8108_Update(void); /* raw 解算物理量 + 刷新 status + 发布快照（控制任务上下文调用） */
void J8108_CopySnapshot(J8108_Snapshot_t *out); /* 任取任务上下文读"帧一致"快照（seqlock 重试） */
const char *J8108_StatusStr(uint8_t status);    /* 状态短标签（屏上显示，ASCII） */
const char *J8108_ErrStr(uint8_t err);          /* 报错位短标签（"OK"/"OVL,OC,..."，ASCII，无错返回 "OK"） */
/* TX 邮箱是否"**连续**占用"超过 limit_ms = 无人 ACK（瞬时占用=正在发送，不算） */
uint8_t J8108_TxStuck(uint32_t now_ms, uint32_t limit_ms);

#ifdef __cplusplus
}
#endif

#endif /* MOTOR_8108_H */
