/*
 * J8108_Task.h - 8108 控制任务对外接口（控制帧发生器 + 外环 + 保护）
 *
 * 架构（docs/协议_串口控制_v1.md §1）：A 板做**外环**，MIT 帧当"力矩接口"。
 *   本模块负责：使能/失能/零点握手（含"发后 20ms 无 ACK 主动丢帧"保护）、心跳看门狗、
 *   保护（跟随误差/堵转/过温/电机报错）、200Hz 控制帧生成、链路边沿日志。
 *   **唯一往 CAN 发帧的地方**（单线程访问 CAN，避免跨任务竞争）。
 *
 * 线程约定：
 *   · 控制帧/握手/看门狗 → 本任务（prio 5）
 *   · 命令解析（CmdRx，prio 4）只**改状态 + 发请求**，不碰 CAN
 *   · 显示/日志/遥测（Monitor，prio 3）只**读快照**
 *
 * 使能握手（异步，防跨任务竞争）：CmdRx 调 J8108_ReqStart() → 本任务执行
 *   发帧 → 等 20ms → 查 ACK → 写结果；CmdRx 轮询 J8108_ReqResult() 后调 J8108_ReqClear()。
 */
#ifndef J8108_TASK_H
#define J8108_TASK_H

#include "stdint.h"
#include "ctrl_core.h"

/* ---- 使能类请求（需要"发帧 + 等 ACK"握手） ---- */
typedef enum
{
    J8108_REQ_NONE = 0,
    J8108_REQ_EN,     /* 使能（…FC）→ 成功后自动进 DAMP 并开始发帧 */
    J8108_REQ_DIS,    /* 失能（…FD） */
    J8108_REQ_ZERO,   /* 位置零点（…FE，掉电保存） */
    J8108_REQ_CLRERR  /* 清除错误位（…FA，双编新增） */
} J8108_Req_e;

typedef enum
{
    J8108_RES_NONE = 0, /* 进行中（未完成） */
    J8108_RES_OK,
    J8108_RES_NOACK,    /* 20ms 内无人 ACK（帧已主动丢弃）→ @ERR 5 */
    J8108_RES_BUSY,     /* CAN 未就绪（INIT FAIL / BUS ERR）→ @ERR 5 */
    J8108_RES_DISABLED  /* 本固件没编译电机模块（FEATURE_J8108=0）→ @ERR 4 feature=j8108 off */
} J8108_Res_e;

void J8108_Task_Init(void);

/* ---- 请求握手（CmdRx 调用） ---- */
void J8108_ReqStart(J8108_Req_e r, const char *why); /* 覆盖式发起；why 仅用于日志 */
J8108_Res_e J8108_ReqResult(void);
void J8108_ReqClear(void);

/* ---- 控制命令（CmdRx 调用；均会自动续心跳） ---- */
void J8108_KeepAlive(void);                     /* 心跳续命（任何合法命令都算"上位机活着"） */
uint8_t J8108_IsEnabled(void);
uint8_t J8108_IsSending(void);                  /* 1=正在发控制帧 */
uint8_t J8108_SetMode(Ctrl_Mode_e m);           /* 1=接受；0=未使能（除 IDLE/DAMP 外）→ @ERR 3 */
void J8108_SetPosDeg(float deg);
void J8108_SetVelDps(float dps);
void J8108_SetVelTff(float tff);                /* 速度环前馈（#V 第 3 参，N·m；不切模式） */
void J8108_SetTorqueNm(float nm);
void J8108_SetImp(float kp, float kd, float tff);
void J8108_SetDampKd(float kd);
void J8108_SetRawMIT(float p_rad, float v_rps, float kp, float kd, float t_nm); /* #SETP：绕过外环直通（需解锁） */
void J8108_StopSoft(void);                      /* #STOP：进阻尼（保持使能，防坠） */
void J8108_StopHard(void);                      /* #ESTOP：停帧 + …FD 失能（输出轴自由） */

/* ---- 状态读取（Monitor / CmdRx 用；只读） ---- */
const Ctrl_State_t *J8108_State(void);
Ctrl_Params_t *J8108_Params(void);              /* #SET / #LIM 需要可写 */
uint32_t J8108_EventLatch(void);
uint32_t J8108_LoopJitterTake(void); /* 取走并清零"控制循环最大间隔 ms"（饿死/长阻塞证据） */                /* 保护事件锁存位（CTRL_EV_*，供屏上显示） */
void J8108_DbgFrames(float *kp, float *kd);     /* 最近一帧的 Kp/Kd（屏上显示） */

/* 模块自检（非破坏性，只读）：0=未编译 1=OK 2=WARN 3=FAIL
 *   J8108_SelfTest()    —— 控制任务/状态机就绪（任务是否起来、设备是否注册）
 *   J8108_CanSelfTest() —— CAN 总线层（INIT_FAIL=3 / BUS_ERR=2 / INIT_OK|READY=1） */
uint8_t J8108_SelfTest(void);
uint8_t J8108_CanSelfTest(void);

#endif /* J8108_TASK_H */
