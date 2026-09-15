/*
 * motor_8108.h - 8108 关节电机（STACKFORCE 单编 24V）CAN 驱动
 *
 * 协议依据：厂商《8108 关节电机用户文档（单编）》（2026-09-15 已摄入知识库）
 *   - 命令帧  ID = 0x000 + CANID（默认 0x01）：数据 FF FF FF FF FF FF FF 码
 *             码：0xFC 使能 / 0xFD 失能 / 0xFE 位置零点 / 0xFB 重启
 *   - MIT 帧  ID = 0x200 + CANID（0x201）：p_des16 | v_des12 | Kp12 | Kd12 | t_ff12（大端位拼装）
 *   - 反馈帧  ID = 0x780 + CANID（0x781）：byte1-2 POS16 / byte3 VEL[11:4] /
 *             byte4 VEL[3:0]|T[11:8] / byte5 T[7:0] / byte6 TMos / byte7 TMotor
 *   - 量程（文档口径）：P ±25.12 rad（电机端）/ V ±45 rad/s（输出端）/
 *             Kp 0~500 / Kd 0~5 / T ±18 N·m；使能后才回传反馈帧
 *   注意：与 DJI 电调（0x200 命令帧体系）ID 不冲突；0x01 命令帧电调会忽略
 */
#ifndef MOTOR_8108_H
#define MOTOR_8108_H

#include "stdint.h"
#include "can.h"
#include "bsp_can.h"

/* ---- 机械/换算参数 ---- */
#define J8108_GEAR_RATIO (8.0f) /* 减速比 8:1（厂商文档）：输出端 = 电机端 ÷ 8 */

/* ---- 协议参数（改 CANID 需与电机参数区一致） ---- */
#define J8108_CAN_ID (0x01u)
#define J8108_CMD_ID (0x000u + J8108_CAN_ID)
#define J8108_MIT_ID (0x200u + J8108_CAN_ID)
#define J8108_FB_ID  (0x780u + J8108_CAN_ID)

/* ---- MIT 量程（厂商文档口径；与电机参数区 PMAX/VMAX/TMAX 对应） ---- */
#define J8108_P_MIN (-25.12f)
#define J8108_P_MAX (25.12f)
#define J8108_V_MIN (-45.0f)
#define J8108_V_MAX (45.0f)
#define J8108_KP_MIN (0.0f)
#define J8108_KP_MAX (500.0f)
#define J8108_KD_MIN (0.0f)
#define J8108_KD_MAX (5.0f)
#define J8108_T_MIN (-18.0f)
#define J8108_T_MAX (18.0f)

/* ---- 命令码 ---- */
#define J8108_CMD_ENABLE (0xFCu)
#define J8108_CMD_DISABLE (0xFDu)
#define J8108_CMD_ZERO (0xFEu)
#define J8108_CMD_REBOOT (0xFBu)

typedef struct
{
    uint8_t raw[8];     /* 最近一帧反馈原始数据 */
    uint32_t rx_count;  /* 累计收到反馈帧数（判链路通断） */
    uint32_t tx_cnt;    /* 累计发送帧数（诊断显示） */
    uint32_t last_rx_ms;/* 最近一帧反馈的接收时刻 ms（失联检测；中断里用 xTaskGetTickCountFromISR） */
    float pos;         /* 电机端位置 rad（输出端 = pos / 8） */
    float vel;         /* 速度 rad/s（按文档口径为输出端） */
    float torque;      /* 力矩 N·m */
    float t_mos;       /* MOS 温度 C */
    float t_rotor;     /* 线圈温度 C */
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
    uint8_t init_ok;        /* CAN 注册+启动结果（1=成功） */
    uint8_t status;         /* J8108_Status_e */
    uint32_t bus_err;       /* 最近一次 HAL_CAN_GetError（0=正常） */
    volatile uint8_t new_frame; /* 中断置位：有新反馈帧待解算（任务侧临界区取走） */
    J8108_Feedback_t fb;
} J8108_t;

/* ---- 数据快照（M2）：给显示/日志用的"帧一致"只读视图 ----
 * 读侧只读快照，不直接读 s_dev：避免 200Hz 控制任务/接收中断与 5Hz 显示任务互相撕数据。 */
typedef struct
{
    uint32_t seq;      /* 序号（偶数=完整；发布时 +2，seqlock） */
    uint8_t valid;     /* 1 = 至少收到过一帧反馈 */
    uint8_t status;    /* J8108_Status_e */
    uint8_t init_ok;
    uint32_t bus_err;
    uint32_t rx_count;
    uint32_t tx_cnt;
    uint32_t frame_age_ms; /* 发布时刻 − 最近一帧时刻（读侧免算） */
    uint8_t raw[8];        /* 与该帧解算值**同帧**的原始字节 */
    float pos;             /* 电机端 rad */
    float vel;             /* rad/s（文档口径：输出端） */
    float torque;          /* N·m */
    float pos_out_deg;     /* 输出端角度（÷8 减速比） */
    float vel_rpm;         /* 输出端转速 RPM */
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
void J8108_SendMIT(float p, float v, float kp, float kd, float t);
void J8108_Update(void); /* raw 解算物理量 + 刷新 status + 发布快照（任务上下文调用） */
void J8108_CopySnapshot(J8108_Snapshot_t *out); /* 任取任务上下文读"帧一致"快照（seqlock 重试） */
const char *J8108_StatusStr(uint8_t status); /* 状态短标签（屏上显示用，ASCII） */

#ifdef __cplusplus
}
#endif

#endif /* MOTOR_8108_H */
