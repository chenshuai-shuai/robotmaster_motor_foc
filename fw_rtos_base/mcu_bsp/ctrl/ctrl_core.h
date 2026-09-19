/*
 * ctrl_core.h - 关节外环控制核（**纯逻辑**：无 HAL/FreeRTOS 依赖 → 宿主机可穷举测试）
 *
 * 设计：A 板做**外环**，8108 的 MIT 帧当作"力矩接口"。
 *   位置模式：纯 PD（把 p_des/Kp/Kd 交给电机内环 PD 执行）
 *   速度模式：外环 PI + 摩擦前馈 → 输出 t_ff（Kp=0, Kd=kd_damp 提供轻微阻尼）
 *   力矩模式：t_ff
 *   阻尼模式：Kp=0, Kd=kd_damp（安全态，失能前的默认态）
 *   阻抗模式：Kp/Kd 可调 + 参考角锁定
 *
 * 单位约定（与串口协议一致）：位置 deg、速度 deg/s、力矩 N·m（**输出端**）
 *   → 本模块输出帧参数时换算成 rad / rad·s⁻¹（输出端口径）。
 *
 * 安全：所有设定点先过"速率限制 → 限幅"，再进帧；保护判定（跟随误差/堵转/超温/电机报错）也在此。
 */
#ifndef CTRL_CORE_H
#define CTRL_CORE_H

#include "stdint.h"

/* ------------------------------- 模式 ------------------------------- */
typedef enum
{
    CTRL_MODE_IDLE = 0, /* 停发帧（只监听） */
    CTRL_MODE_DAMP,     /* 阻尼保持（安全态） */
    CTRL_MODE_SPEED,    /* 速度（外环 PI + 前馈） */
    CTRL_MODE_POS,      /* 位置（纯 PD） */
    CTRL_MODE_TORQUE,   /* 力矩 */
    CTRL_MODE_IMP       /* 阻抗 （锁参考角） */
} Ctrl_Mode_e;

/* --------------------------- 保护事件位 --------------------------- */
#define CTRL_EV_TIMEOUT (1u << 0)  /* 心跳超时 → 已退 DAMP */
#define CTRL_EV_LIMIT (1u << 1)    /* 设定点被限位/限幅夹紧 */
#define CTRL_EV_FOLLOW (1u << 2)   /* 跟随误差超限 */
#define CTRL_EV_STALL (1u << 3)    /* 堵转（大力矩+低转速持续） */
#define CTRL_EV_TEMP (1u << 4)     /* 过温（预警或停机） */
#define CTRL_EV_MOTERR (1u << 5)   /* 电机报错位非 0 */
#define CTRL_EV_ESTOP (1u << 6)    /* 收到硬急停 */

/* 限幅来源（CTRL_EV_LIMIT 的细节，供 @EVT 文本用） */
#define CTRL_LIM_NONE (0u)
#define CTRL_LIM_SETPOINT (1u) /* 设定点被软限位/限速夹紧 */
#define CTRL_LIM_TORQUE (2u)   /* 力矩被限幅（TMAX）夹紧 */
#define CTRL_LIM_TRATE (3u)    /* 力矩被**斜率限制**（TRATE）夹紧 */

/* 事件"消退去抖"时间：受限条件必须**连续消失**这么久，才允许把下一次受限算作"新事件"。
 * 为什么需要：斜率限制会"时紧时松"地抖（PI 输出变化快慢交替），纯边沿锁存会被反复解锁 → 刷屏 */
#define CTRL_EV_CLEAR_MS (1000u)

/* ------------------------------- 参数 ------------------------------- */
typedef struct
{
    /* 增益 */
    float kd_damp;   /* 阻尼系数 0~5（过大震荡，文档警告） */
    float kp_pos;    /* 位置增益 0~500（**必须配 kd_pos**，Kp≠0&Kd=0 会震荡/失控） */
    float kd_pos;    /* 位置阻尼 0~5 */
    float kp_v;      /* 速度环比例 N·m per (deg/s) */
    float ki_v;      /* 速度环积分 N·m per (deg/s·s) */
    float kp_imp;    /* 阻抗刚度 */
    float kd_imp;    /* 阻抗阻尼 */
    /* 判定/限幅（输出端单位） */
    float inpos_deg;  /* 到位判定带 ±deg */
    float pmin_deg;   /* 软限位下限 */
    float pmax_deg;   /* 软限位上限 */
    float vmax_dps;   /* 速度上限 deg/s */
    float tmax_nm;    /* 力矩上限 N·m */
    float rate_dps2;  /* 设定点变化率上限 deg/s² */
    float tff_nm;     /* **摩擦前馈** N·m（库仑摩擦补偿）：按速度设定点方向自动取正负；
                       * 静摩擦大的关节必须先垫上这个，否则全靠积分攒 → 起步要几十秒（实机 2026-09-18）*/
    float trate_nm_s; /* **力矩变化率上限 N·m/s**（0=不限）。小惯量直驱关节必须限：一次给一大步力矩
                       * 会让关节在一个 5ms 周期内加速上百 dps → 极限环震荡（实机 2026-09-18 抖动的根因）*/
    /* 保护阈值 */
    float follow_deg;    /* 跟随误差阈值 deg */
    uint16_t follow_ms;  /* 跟随误差持续时间 ms */
    float stall_nm;      /* 堵转力矩阈值 N·m */
    float stall_dps;     /* 堵转转速阈值 deg/s */
    uint16_t stall_ms;   /* 堵转持续时间 ms */
    float temp_warn_c;   /* 过温预警 ℃ */
    float temp_stop_c;   /* 过温停机 ℃ */
    uint16_t wd_ms;      /* 心跳超时 ms（0=关） */
    /* 开关 */
    uint8_t autodamp_on_err; /* 电机报错位非 0 → 自动退 DAMP */
    uint8_t setp_unlocked;   /* #SETP 调试直通解锁 */
} Ctrl_Params_t;

/* ------------------------------- 状态 ------------------------------- */
typedef struct
{
    Ctrl_Mode_e mode;
    uint8_t enabled;      /* 使能状态（由 #EN/#DIS 驱动） */
    float set;            /* 目标设定点（deg / deg·s⁻¹ / N·m，按模式） */
    float set_applied;    /* 经速率限制后的已应用设定点 */
    float tff;            /* 附加前馈（N·m，来自命令参数） */
    float t_cmd;          /* 上一次**实际下发**的力矩（力矩斜率限制的状态量） */
    float integ;          /* 速度环积分累积（N·m） */
    float imp_ref_deg;    /* 阻抗参考角 */
    uint8_t inpos;        /* 到位标志（上升沿报一次） */
    /* 事件边沿锁存 + 消退去抖（**持续为真的条件只报一次**，防 @EVT 洪泛把串口占满 —— 实机教训） */
    uint8_t lim_latched;        /* 限幅事件已报（条件连续消失 1s 后重新武装） */
    uint8_t lim_what;           /* 本次夹紧来源：CTRL_LIM_SETPOINT / CTRL_LIM_TORQUE / CTRL_LIM_TRATE */
    uint8_t temp_warn_latched;  /* 过温预警已报（温度回落 -5℃ 后复位） */
    uint8_t moterr_latched;     /* 电机报错已报（报错位清 0 后复位） */
    uint16_t lim_clear_ms;      /* 受限条件"连续消失"计时（去抖，见 CTRL_EV_CLEAR_MS） */
    uint16_t moterr_clear_ms;   /* 报错位"连续为 0"计时（去抖） */
    uint32_t last_cmd_ms; /* 最近一次"续命"时刻（心跳） */
    uint16_t ev;          /* 保护事件位（CTRL_EV_*，由 Ctrl_TakeEvents 取走） */
    uint32_t follow_acc_ms; /* 跟随误差累计时长 */
    uint32_t stall_acc_ms;  /* 堵转累计时长 */
    uint8_t estop;          /* 硬急停已置位（需 #EN 复位） */
} Ctrl_State_t;

/* 每周期算出的"要发给电机的帧参数"（**输出端口径**：rad / rad·s⁻¹ / N·m） */
typedef struct
{
    float p_rad;
    float v_rps;
    float kp;
    float kd;
    float t_nm;
    uint8_t send; /* 0=本周期不发帧（IDLE / 未使能） */
} Ctrl_Frame_t;

#ifdef __cplusplus
extern "C"
{
#endif

void Ctrl_ParamsDefault(Ctrl_Params_t *p);
void Ctrl_Init(Ctrl_State_t *s, const Ctrl_Params_t *p);

/* 切模式：目标非 IDLE 时，把 set_applied 预置为当前反馈值（**防跳变**：先原地接手再给新目标） */
void Ctrl_SetMode(Ctrl_State_t *s, Ctrl_Mode_e m, float fb_pos_deg, float fb_vel_dps);
void Ctrl_SetPos(Ctrl_State_t *s, float deg);
void Ctrl_SetVel(Ctrl_State_t *s, float dps);
void Ctrl_SetTorque(Ctrl_State_t *s, float nm);
void Ctrl_SetTff(Ctrl_State_t *s, float tff); /* 速度环前馈（#V 第 3 参），不切模式 */
void Ctrl_SetImp(Ctrl_State_t *s, Ctrl_Params_t *p, float kp, float kd, float tff);
void Ctrl_SetDamp(Ctrl_State_t *s, Ctrl_Params_t *p, float kd);
void Ctrl_Release(Ctrl_State_t *s); /* 退出运动模式 → 回 DAMP */
void Ctrl_Ping(Ctrl_State_t *s, uint32_t now_ms);
void Ctrl_Estop(Ctrl_State_t *s);   /* 硬急停：停帧 + 置标志 */
uint8_t Ctrl_TakeEvents(Ctrl_State_t *s); /* 取走事件位（读后清零） */

/* 心跳超时检查：超时则强制退 DAMP（安全，不失能），返回 1=本次刚超时 */
uint8_t Ctrl_Heartbeat(Ctrl_State_t *s, const Ctrl_Params_t *p, uint32_t now_ms);

/* 保护检查（跟随误差/堵转/过温/电机报错）→ 累计事件位；可能把模式强制切 DAMP。
 * 返回本轮新增事件位（调用方决定是否发 @EVT 日志） */
uint16_t Ctrl_Protect(Ctrl_State_t *s, const Ctrl_Params_t *p, float pos_deg, float vel_dps,
                      float t_nm, float t_mos, float t_rotor, uint8_t motor_err, uint32_t dt_ms);

/* 控制核一步：算出本周期帧参数（含速率限制 + 限幅 + PI/PD） */
void Ctrl_Step(Ctrl_State_t *s, const Ctrl_Params_t *p, float fb_pos_deg, float fb_vel_dps,
               uint32_t dt_ms, Ctrl_Frame_t *out);

const char *Ctrl_ModeStr(Ctrl_Mode_e m);
const char *Ctrl_LimitWhat(const Ctrl_State_t *s); /* "setpoint" / "torque" / "none" */

#ifdef __cplusplus
}
#endif

#endif /* CTRL_CORE_H */
