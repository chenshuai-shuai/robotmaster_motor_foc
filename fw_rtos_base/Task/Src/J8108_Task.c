/*
 * J8108_Task.c - 8108 控制任务（控制帧发生器 + 外环 + 保护）
 *
 * 职责（详见 J8108_Task.h 与 docs/协议_串口控制_v1.md）：
 *   1. 200Hz 控制帧生成：Ctrl_Step() 算出 (p,v,Kp,Kd,t_ff) → J8108_SendMIT()
 *   2. 使能/失能/零点握手：发帧 → 等 20ms → 查 ACK（本板 AutoRetransmission=ENABLE，
 *      无人应答会无限重传 → 必须主动 HAL_CAN_AbortTxRequest 丢帧）
 *   3. 心跳看门狗：200ms 无命令 → 退 DAMP（**不失能**：本电机无抱闸，失能会自由坠落）
 *   4. 保护：跟随误差 / 堵转 / 过温 / 电机报错位 → @EVT + 退阻尼
 *   5. 链路日志：只在**状态边沿**与**数据新鲜**时打印（绝不重播旧数据）
 *
 * 线程：本任务 prio 5（最高）保时；屏刷/日志/遥测在 Monitor（prio 3）——绝不在控制路径上。
 * 说明：**本文件是唯一往 CAN 发帧的地方**（单线程访问，杜绝跨任务竞争）。
 */
#include "J8108_Task.h"

#include "FreeRTOS.h"
#include "task.h"

#include "bsp_log.h"
#include "motor_8108.h"
#include "ctrl_core.h"
#include "proto_tx.h"  /* @EVT 事件上报（M2：去掉 ui_action.h 反向依赖 —— 控制层不该依赖 UI 层） */
#include "version.h"
#include "main.h"
#include "can.h"       /* hcan1：J8108_Task_Init 里必须做 CAN 注册 */
#include "stack_probe.h" /* 栈余量自报 */
#include "feature_config.h"

#include <stdio.h>
#include <string.h>

#if FEATURE_J8108   /* M2：实现全文被宏包住（未编译时走文件末尾的"显式停用桩"） */

/* ------------------------------ 任务参数 ------------------------------ */
#define J8108_TASK_PRIORITY (5U)
#define J8108_TASK_STACK_WORDS (640U)
#define J8108_CTRL_PERIOD_MS (5U)    /* 控制帧周期 200Hz */
#define J8108_EN_ACK_MS (20U)        /* 使能帧后等 ACK 的时长（1Mbps 单帧 ~130µs，20ms 极宽裕） */
#define J8108_WD_POLL_MS (20U)       /* 帧/链路看门狗轮询周期 */
#define J8108_TX_STUCK_MS (100U)     /* TX 邮箱"连续占用"超此值 = 无人 ACK */
#define J8108_FB_LOST_MS (500U)      /* 反馈超时 = 链路丢 */
#define J8108_FB_FRESH_MS (200U)     /* 数据新鲜门槛（屏/日志/遥测统一） */
#define J8108_RETRY_MS (500U)        /* 自动恢复退避基数（n×基数） */
#define J8108_RETRY_MAX (4U)         /* 连续失败上限：超过则放弃并提示（用户 `#EN` 重来） */

static TaskHandle_t s_task_handle;
static J8108_t *s_dev;
static Ctrl_State_t s_ctrl;
static Ctrl_Params_t s_params;

static uint8_t s_link_ok = 0u;
static uint8_t s_status_dbg = 0xFFu;
static uint32_t s_last_rx_dbg = 0u;
static uint32_t s_ev_latch = 0u;

/* 使能类请求握手 */
static volatile uint8_t s_req = (uint8_t)J8108_REQ_NONE;
static volatile uint8_t s_res = (uint8_t)J8108_RES_NONE;
static volatile uint8_t s_req_fresh = 0u; /* 1=请求刚发起，需要执行发送阶段 */
static uint32_t s_req_sent_ms = 0u;

/* 自动恢复（掉线后重试使能握手） */
static uint8_t s_fail = 0u;
static uint8_t s_want_recover = 0u;
static uint32_t s_retry_ms = 0u;
static uint32_t s_hold_rx_seen = 0u;

/* 发送节拍 */
static uint32_t s_tx_ms = 0u;
/* 控制循环"最大间隔"（抖动/饿死证据）：10s 窗口内的最大 dt，由 Monitor 取走打印 */
static volatile uint32_t s_dt_max_ms = 0u;
static uint32_t s_wd_ms = 0u;
static float s_kp_dbg = 0.0f;
static float s_kd_dbg = 0.0f;

static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static CAN_HandleTypeDef *j8108_can(void)
{
    return (s_dev->can_instance != NULL) ? s_dev->can_instance->can_handle : NULL;
}

/* --------------------------- 请求握手 API（CmdRx 调用） --------------------------- */
void J8108_ReqStart(J8108_Req_e r, const char *why)
{
    s_req = (uint8_t)r;
    s_res = (uint8_t)J8108_RES_NONE;
    s_req_fresh = 1u;
    s_req_sent_ms = 0u;
    LOG_I("J8108", "request queued: %s (%s)",
          (r == J8108_REQ_EN) ? "ENABLE" : ((r == J8108_REQ_DIS) ? "DISABLE" : "ZERO"),
          (why != NULL) ? why : "-");
}

J8108_Res_e J8108_ReqResult(void)
{
    return (J8108_Res_e)s_res;
}

void J8108_ReqClear(void)
{
    s_res = (uint8_t)J8108_RES_NONE;
    s_req = (uint8_t)J8108_REQ_NONE;
}

/* ------------------------------ 控制命令 API ------------------------------ */
/* #SETP 原始直通：#SET SETP 1 解锁后生效；任何常规运动命令都会自动退出直通（防"忘了还开着"）*/
static uint8_t s_raw_on = 0u;
static float s_raw[5];

void J8108_SetRawMIT(float p_rad, float v_rps, float kp, float kd, float t_nm)
{
    s_raw[0] = p_rad;
    s_raw[1] = v_rps;
    s_raw[2] = kp;
    s_raw[3] = kd;
    s_raw[4] = t_nm;
    s_raw_on = 1u;
    LOG_W("J8108", "SETP raw passthrough ON: p=%.4f v=%.3f kp=%.1f kd=%.2f t=%.3f (motor units, NO clamping)",
          (double)p_rad, (double)v_rps, (double)kp, (double)kd, (double)t_nm);
}

static void raw_off(void)
{
    if (s_raw_on != 0u)
    {
        s_raw_on = 0u;
        LOG_I("J8108", "SETP raw passthrough OFF (back to outer-loop control)");
    }
}

void J8108_KeepAlive(void)
{
    Ctrl_Ping(&s_ctrl, now_ms());
}

uint8_t J8108_IsEnabled(void)
{
    return s_ctrl.enabled;
}

uint8_t J8108_IsSending(void)
{
    return (uint8_t)((s_ctrl.enabled != 0u) && (s_ctrl.mode != CTRL_MODE_IDLE));
}

uint8_t J8108_SetMode(Ctrl_Mode_e m)
{
    if ((m != CTRL_MODE_IDLE) && (m != CTRL_MODE_DAMP) && (s_ctrl.enabled == 0u))
    {
        return 0u; /* 运动模式必须先使能 */
    }
    raw_off(); /* 切模式即退出 #SETP 直通 */
    if (m == CTRL_MODE_IDLE)
    {
        s_ctrl.mode = CTRL_MODE_IDLE; /* 停发帧（保持使能状态，靠 #EN/#DIS 改） */
        return 1u;
    }
    Ctrl_SetMode(&s_ctrl, m, s_dev->fb.pos * J8108_RAD2DEG, s_dev->fb.vel * J8108_RAD2DEG);
    return 1u;
}

void J8108_SetPosDeg(float deg)
{
    raw_off();
    /* ★ 关键：先经 Ctrl_SetMode 把 set_applied 预置为**当前实际角**。
     * 否则 set_applied 会保留上一个模式的值（如 DAMP 的 0），slew 从 0 开始 →
     * 与真实位置的巨大 PD 误差 → 猛冲（审查抓到的安全问题）。 */
    Ctrl_SetMode(&s_ctrl, CTRL_MODE_POS, s_dev->fb.pos * J8108_RAD2DEG, s_dev->fb.vel * J8108_RAD2DEG);
    Ctrl_SetPos(&s_ctrl, deg);
}

void J8108_SetVelDps(float dps)
{
    raw_off();
    Ctrl_SetMode(&s_ctrl, CTRL_MODE_SPEED, s_dev->fb.pos * J8108_RAD2DEG, s_dev->fb.vel * J8108_RAD2DEG);
    Ctrl_SetVel(&s_ctrl, dps); /* 速度设定点从 0 起、按 #RATE 斜坡 */
}

void J8108_SetVelTff(float tff)
{
    Ctrl_SetTff(&s_ctrl, tff); /* 前馈不切模式（#V 第 3 参） */
}

void J8108_SetTorqueNm(float nm)
{
    raw_off();
    Ctrl_SetMode(&s_ctrl, CTRL_MODE_TORQUE, s_dev->fb.pos * J8108_RAD2DEG, s_dev->fb.vel * J8108_RAD2DEG);
    Ctrl_SetTorque(&s_ctrl, nm);
}

void J8108_SetImp(float kp, float kd, float tff)
{
    raw_off();
    /* 阻抗参考角 = 进入时**实际角**（先 Ctrl_SetMode 再 SetImp） */
    Ctrl_SetMode(&s_ctrl, CTRL_MODE_IMP, s_dev->fb.pos * J8108_RAD2DEG, s_dev->fb.vel * J8108_RAD2DEG);
    Ctrl_SetImp(&s_ctrl, &s_params, kp, kd, tff);
}

void J8108_SetDampKd(float kd)
{
    raw_off();
    Ctrl_SetDamp(&s_ctrl, &s_params, kd);
}

void J8108_StopSoft(void)
{
    Ctrl_Release(&s_ctrl);
}

void J8108_StopHard(void)
{
    raw_off();
    Ctrl_Estop(&s_ctrl);                              /* enabled=0 → 立即停发控制帧 */
    J8108_ReqStart(J8108_REQ_DIS, "estop");           /* 失能帧由控制任务在本任务上下文发出 */
    s_ev_latch |= CTRL_EV_ESTOP;
    LOG_W("J8108", "HARD ESTOP: frames off + DISABLE queued (output shaft will be FREE - no brake!)");
}

const Ctrl_State_t *J8108_State(void)
{
    return &s_ctrl;
}

Ctrl_Params_t *J8108_Params(void)
{
    return &s_params;
}

uint32_t J8108_LoopJitterTake(void)
{
    uint32_t v = s_dt_max_ms;

    s_dt_max_ms = 0u; /* 读后清零：每个窗口独立统计 */
    return v;
}

uint32_t J8108_EventLatch(void)
{
    return s_ev_latch;
}

void J8108_DbgFrames(float *kp, float *kd)
{
    if (kp != NULL)
        *kp = s_kp_dbg;
    if (kd != NULL)
        *kd = s_kd_dbg;
}

/* ------------------------------ 事件上报 ------------------------------ */
static void ev_report(uint16_t ev)
{
    char b[96];

    if (ev == 0u)
        return;
    if ((ev & CTRL_EV_TIMEOUT) != 0u)
    {
        LOG_W("J8108", "EVT TIMEOUT: no command for %ums -> back to DAMP (motor stays ENABLED)", (unsigned)s_params.wd_ms);
        Proto_Send("@EVT TIMEOUT mode=DAMP");
    }
    if ((ev & CTRL_EV_LIMIT) != 0u)
    {
        (void)snprintf(b, sizeof(b), "@EVT LIMIT what=%s", Ctrl_LimitWhat(&s_ctrl));
        Proto_Send(b);
    }
    if ((ev & CTRL_EV_FOLLOW) != 0u)
    {
        LOG_W("J8108", "EVT FOLLOW: position error too large -> DAMP");
        Proto_Send("@EVT FOLLOW mode=DAMP");
    }
    if ((ev & CTRL_EV_STALL) != 0u)
    {
        LOG_W("J8108", "EVT STALL: high torque + low speed -> DAMP");
        Proto_Send("@EVT STALL mode=DAMP");
    }
    if ((ev & CTRL_EV_TEMP) != 0u)
    {
        (void)snprintf(b, sizeof(b), "@EVT TEMP Tm=%.1f Tr=%.1f warn=%.0f stop=%.0f",
                       (double)s_dev->fb.t_mos, (double)s_dev->fb.t_rotor,
                       (double)s_params.temp_warn_c, (double)s_params.temp_stop_c);
        LOG_W("J8108", "EVT TEMP: Tm=%.1f Tr=%.1f (warn %.0f / stop %.0f)",
              (double)s_dev->fb.t_mos, (double)s_dev->fb.t_rotor, (double)s_params.temp_warn_c, (double)s_params.temp_stop_c);
        Proto_Send(b);
    }
    if ((ev & CTRL_EV_MOTERR) != 0u)
    {
        LOG_E("J8108", "EVT MOTOR ERR: byte0=0x%02X (%s)", s_dev->fb.err, J8108_ErrStr(s_dev->fb.err));
        (void)snprintf(b, sizeof(b), "@EVT ERR=0x%02X what=%s", s_dev->fb.err, J8108_ErrStr(s_dev->fb.err));
        Proto_Send(b);
    }
    if ((ev & CTRL_EV_ESTOP) != 0u)
    {
        Proto_Send("@EVT ESTOP mode=DISABLED");
    }
}

/* --------------------------- 使能/失能/零点 握手 --------------------------- */
static void j8108_req_poll(uint32_t t)
{
    CAN_HandleTypeDef *h = j8108_can();
    char b[96];

    /* 自动恢复：掉线后按退避重试"使能握手" */
    if ((s_want_recover != 0u) && (s_ctrl.enabled == 0u) &&
        ((int32_t)(t - s_retry_ms) >= 0) && (s_req == (uint8_t)J8108_REQ_NONE))
    {
        J8108_ReqStart(J8108_REQ_EN, "auto-resume");
    }

    if (s_req == (uint8_t)J8108_REQ_NONE)
    {
        return;
    }

    /* CAN 未就绪：直接失败（不重试，用户要求） */
    if ((s_dev->init_ok == 0u) || (s_dev->status == (uint8_t)J8108_ST_BUS_ERR))
    {
        s_res = (uint8_t)J8108_RES_BUSY;
        s_req_fresh = 0u;
        LOG_E("J8108", "request rejected: CAN not ready (status=%s)", J8108_StatusStr(s_dev->status));
        return;
    }
    if (h == NULL)
    {
        s_res = (uint8_t)J8108_RES_BUSY;
        s_req_fresh = 0u;
        return;
    }

    /* 阶段 1：发送 */
    if (s_req_fresh != 0u)
    {
        s_req_fresh = 0u;
        if (s_req == (uint8_t)J8108_REQ_EN)
        {
            LOG_I("J8108", "-> ENABLE frame (0x%03X id, data ...FC) | wait %ums for ACK", J8108_CMD_ID, (unsigned)J8108_EN_ACK_MS);
            J8108_SendCmd(J8108_CMD_ENABLE);
        }
        else if (s_req == (uint8_t)J8108_REQ_DIS)
        {
            s_ctrl.enabled = 0u; /* 立即停发控制帧，腾空邮箱给失能帧 */
            s_ctrl.mode = CTRL_MODE_IDLE;
            LOG_I("J8108", "-> DISABLE frame (data ...FD) | wait %ums for ACK", (unsigned)J8108_EN_ACK_MS);
            J8108_SendCmd(J8108_CMD_DISABLE);
        }
        else if (s_req == (uint8_t)J8108_REQ_ZERO)
        {
            LOG_I("J8108", "-> ZERO frame (data ...FE, power-off persistent) | wait %ums for ACK", (unsigned)J8108_EN_ACK_MS);
            J8108_SendCmd(J8108_CMD_ZERO);
        }
        else
        {
            LOG_I("J8108", "-> CLEAR-ERR frame (data ...FA) | wait %ums for ACK", (unsigned)J8108_EN_ACK_MS);
            J8108_SendCmd(J8108_CMD_CLEAR_ERR);
        }
        s_req_sent_ms = t;
        return;
    }

    /* 阶段 2：等 ACK 结果 */
    if ((uint32_t)(t - s_req_sent_ms) < (uint32_t)J8108_EN_ACK_MS)
    {
        return;
    }
    if (J8108_TxStuck(t, J8108_EN_ACK_MS) != 0u)
    {
        (void)HAL_CAN_AbortTxRequest(h, CAN_TX_MAILBOX0 | CAN_TX_MAILBOX1 | CAN_TX_MAILBOX2);
        s_res = (uint8_t)J8108_RES_NOACK;
        LOG_W("J8108", "no ACK in %ums -> TX aborted. check 24V / CAN_H-L / 120R / CANID=0x%02X", (unsigned)J8108_EN_ACK_MS, J8108_CAN_ID);
        return;
    }

    s_res = (uint8_t)J8108_RES_OK;
    if (s_req == (uint8_t)J8108_REQ_EN)
    {
        s_ctrl.enabled = 1u;
        s_ctrl.estop = 0u;
        s_fail = 0u;
        s_want_recover = 0u;
        Ctrl_SetMode(&s_ctrl, CTRL_MODE_DAMP, s_dev->fb.pos * J8108_RAD2DEG, s_dev->fb.vel * J8108_RAD2DEG);
        Ctrl_Ping(&s_ctrl, t);
        s_tx_ms = 0u; /* 下一轮立即发第一帧 */
        LOG_I("J8108", "ENABLE ok -> mode=DAMP, control frames ON (200Hz) -> feedback should stream now");
        Proto_Send("@EVT ENABLED mode=DAMP");
    }
    else if (s_req == (uint8_t)J8108_REQ_DIS)
    {
        s_want_recover = 0u;
        LOG_I("J8108", "DISABLE ok -> frames off, motor de-energized (shaft FREE)");
        Proto_Send("@EVT DISABLED");
    }
    else if (s_req == (uint8_t)J8108_REQ_ZERO)
    {
        LOG_I("J8108", "ZERO ok -> current position stored as zero (persistent)");
        Proto_Send("@EVT ZERO");
    }
    else
    {
        LOG_I("J8108", "CLEAR-ERR ok -> motor error bits cleared (byte0=0x%02X now)", s_dev->fb.err);
        (void)snprintf(b, sizeof(b), "@EVT CLRERR err=0x%02X", s_dev->fb.err);
        Proto_Send(b);
    }
}

/* --------------------------- 心跳 + 保护 --------------------------- */
static void j8108_mode_poll(uint32_t t, uint32_t dt)
{
    uint16_t ev;

    /* 心跳看门狗（仅使能时才有意义） */
    if (s_ctrl.enabled != 0u)
    {
        if (Ctrl_Heartbeat(&s_ctrl, &s_params, t) != 0u)
        {
            raw_off(); /* 心跳超时退阻尼，同时退出 #SETP 直通 */
        }
    }

    /* 保护检查：跟随误差 / 堵转 / 过温 / 电机报错位（**单一入口**，用最新反馈值） */
    ev = Ctrl_Protect(&s_ctrl, &s_params,
                      s_dev->fb.pos * J8108_RAD2DEG,
                      s_dev->fb.vel * J8108_RAD2DEG,
                      s_dev->fb.torque,
                      s_dev->fb.t_mos, s_dev->fb.t_rotor,
                      s_dev->fb.err, dt);
    ev = (uint16_t)(ev | Ctrl_TakeEvents(&s_ctrl));
    if (ev != 0u)
    {
        s_ev_latch |= (uint32_t)ev;
        ev_report(ev);
    }
}

/* --------------------------- 控制帧生成 --------------------------- */
static void j8108_frame_poll(uint32_t t, uint32_t dt)
{
    CAN_HandleTypeDef *h = j8108_can();
    Ctrl_Frame_t fr;
    uint32_t rx = s_dev->fb.rx_count;

    /* ---- 1. 帧看门狗：任何时刻都查（即使已停发 → 抓"急停帧/失能帧"卡死） ---- */
    if (h != NULL)
    {
        if ((int32_t)(t - s_wd_ms) >= 0)
        {
            s_wd_ms = t + J8108_WD_POLL_MS;

            if (J8108_TxStuck(t, J8108_TX_STUCK_MS) != 0u)
            {
                (void)HAL_CAN_AbortTxRequest(h, CAN_TX_MAILBOX0 | CAN_TX_MAILBOX1 | CAN_TX_MAILBOX2);
                LOG_W("J8108", "TX not ACKed (mailbox stuck %ums) -> aborted; check 24V/CAN_H-L/120R/CANID",
                      (unsigned)J8108_TX_STUCK_MS);
                if (s_ctrl.enabled != 0u)
                {
                    s_ctrl.enabled = 0u;
                    s_ctrl.mode = CTRL_MODE_IDLE;
                    s_fail++;
                    s_retry_ms = t + (uint32_t)J8108_RETRY_MS * (uint32_t)s_fail;
                    if (s_fail >= J8108_RETRY_MAX)
                    {
                        s_want_recover = 0u;
                        LOG_E("J8108", "no ACK %u times -> give up retrying; send #EN to try again", (unsigned)J8108_RETRY_MAX);
                        Proto_Send("@EVT LINKLOST what=noack");
                    }
                    else
                    {
                        s_want_recover = 1u;
                        LOG_W("J8108", "link lost (no ACK) -> will retry ENABLE in %ums (%u/%u)",
                              (unsigned)(J8108_RETRY_MS * s_fail), (unsigned)s_fail, (unsigned)J8108_RETRY_MAX);
                        Proto_Send("@EVT LINKLOST what=noack");
                    }
                }
            }
            else if ((s_ctrl.enabled != 0u) && (rx > 0u) && ((uint32_t)(t - s_dev->fb.last_rx_ms) > J8108_FB_LOST_MS))
            {
                s_ctrl.enabled = 0u;
                s_ctrl.mode = CTRL_MODE_IDLE;
                s_fail++;
                s_retry_ms = t + (uint32_t)J8108_RETRY_MS * (uint32_t)s_fail;
                if (s_fail >= J8108_RETRY_MAX)
                {
                    s_want_recover = 0u;
                    LOG_E("J8108", "no feedback %u times -> stopped; check motor power/state; send #EN to retry", (unsigned)J8108_RETRY_MAX);
                }
                else
                {
                    s_want_recover = 1u;
                    LOG_W("J8108", "no feedback for %ums -> retry ENABLE in %ums (%u/%u)",
                          (unsigned)J8108_FB_LOST_MS, (unsigned)(J8108_RETRY_MS * s_fail), (unsigned)s_fail, (unsigned)J8108_RETRY_MAX);
                }
                Proto_Send("@EVT LINKLOST what=nofb");
            }
            else if (rx != s_hold_rx_seen)
            {
                s_hold_rx_seen = rx;
                if (s_fail != 0u)
                {
                    LOG_I("J8108", "link healthy again (rx=%lu) -> failure counter reset", (unsigned long)rx);
                    s_fail = 0u;
                }
            }
        }
    }

    /* ---- 2. 生成帧参数（#SETP 直通优先；否则走外环） ---- */
    if ((s_raw_on != 0u) && (s_ctrl.enabled == 0u))
    {
        raw_off(); /* 停帧状态下自动退出直通 */
    }
    if (s_raw_on != 0u)
    {
        fr.p_rad = s_raw[0];
        fr.v_rps = s_raw[1];
        fr.kp = s_raw[2];
        fr.kd = s_raw[3];
        fr.t_nm = s_raw[4];
        fr.send = 1u;
    }
    else
    {
        Ctrl_Step(&s_ctrl, &s_params,
                  s_dev->fb.pos * J8108_RAD2DEG, s_dev->fb.vel * J8108_RAD2DEG, dt, &fr);
    }
    s_kp_dbg = fr.kp;
    s_kd_dbg = fr.kd;

    /* ---- 3. 发送（200Hz；邮箱无空位就先不发，交看门狗处理） ---- */
    if ((fr.send != 0u) && (h != NULL) && ((uint32_t)(t - s_tx_ms) >= (uint32_t)J8108_CTRL_PERIOD_MS) &&
        (HAL_CAN_GetTxMailboxesFreeLevel(h) > 0u))
    {
        s_tx_ms = t;
        J8108_SendMIT(fr.p_rad, fr.v_rps, fr.kp, fr.kd, fr.t_nm);
    }
}

/* --------------------------- 链路日志（边沿 + 新鲜数据） --------------------------- */
static void j8108_link_debug(uint32_t t)
{
    uint8_t now_ok = 0u;

    if (s_dev->fb.rx_count > 0u)
    {
        now_ok = ((uint32_t)(t - s_dev->fb.last_rx_ms) < (uint32_t)J8108_FB_FRESH_MS) ? 1u : 0u;
    }

    if (now_ok != s_link_ok)
    {
        s_link_ok = now_ok;
        if (now_ok != 0u)
        {
            LOG_I("J8108", "CAN feedback link UP (rx=%lu) -> motor powered AND enabled", (unsigned long)s_dev->fb.rx_count);
        }
        else
        {
            LOG_W("J8108", "CAN feedback link LOST last_dt=%lums (motor disabled/power off/wiring)",
                  (unsigned long)(t - s_dev->fb.last_rx_ms));
        }
    }

    if (s_dev->status != s_status_dbg)
    {
        s_status_dbg = s_dev->status;
        switch (s_status_dbg)
        {
        case J8108_ST_INIT_FAIL:
            LOG_E("J8108", "CAN status -> INIT FAIL (no retry by design): check CAN peripheral / device table");
            break;
        case J8108_ST_INIT_OK:
            LOG_I("J8108", "CAN status -> INIT OK / WAITING for node");
            break;
        case J8108_ST_READY:
            LOG_I("J8108", "CAN status -> READY (feedback arriving, channel established)");
            break;
        case J8108_ST_BUS_ERR:
            LOG_W("J8108", "CAN status -> BUS ERR (HAL err=0x%04lX): check ACK/wiring/120R", (unsigned long)s_dev->bus_err);
            break;
        default:
            break;
        }
    }

#if FEATURE_VERBOSE_LOG
    if ((s_dev->fb.rx_count != s_last_rx_dbg) && (s_dev->fb.rx_count <= 10u))
    {
        const uint8_t *d = s_dev->fb.raw;

        s_last_rx_dbg = s_dev->fb.rx_count;
        LOG_D("J8108", "fb#%lu raw %02X %02X %02X %02X %02X %02X %02X %02X | pos=%.2fdeg vel=%.1fdps T=%.3fNm Tm=%.1f Tr=%.1f err=0x%02X",
              (unsigned long)s_dev->fb.rx_count, d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7],
              (double)(s_dev->fb.pos * J8108_RAD2DEG), (double)(s_dev->fb.vel * J8108_RAD2DEG),
              (double)s_dev->fb.torque, (double)s_dev->fb.t_mos, (double)s_dev->fb.t_rotor, s_dev->fb.err);
    }
#endif
}

/* ------------------------------ 任务主体 ------------------------------ */
static void j8108_task(void *arg)
{
    uint32_t prev_ms = now_ms();

    (void)arg;

    LOG_I("J8108", "========================================");
    LOG_I("J8108", "8108 joint motor control task | FW %s | proto v1 (serial-driven)", FW_VERSION_STR);

#if J8108_SCALE_OUTPUT_SIDE
    LOG_I("J8108", "SCALE = OUTPUT-SIDE (dual-encoder doc): pos/vel are joint-side, NO /8. pmax=%.2frad(%.0fdeg) vmax=%.0f tmax=%.1f dir=%+.0f",
          (double)J8108_P_HI, (double)(J8108_P_HI * J8108_RAD2DEG), (double)J8108_V_HI, (double)J8108_T_HI, (double)J8108_DIR);
#else
    LOG_I("J8108", "SCALE = MOTOR-SIDE fallback (single-encoder): output = pos / %.0f. pmax=%.2frad vmax=%.0f tmax=%.1f",
          (double)J8108_GEAR_RATIO, (double)J8108_P_HI, (double)J8108_V_HI, (double)J8108_T_HI);
#endif
    LOG_I("J8108", "ctrl: CAN1 1Mbps | cmd 0x%03X | MIT 0x%03X | FB 0x%03X | %uHz frames | wd=%ums",
          J8108_CMD_ID, J8108_MIT_ID, J8108_FB_ID, (unsigned)(1000u / J8108_CTRL_PERIOD_MS), (unsigned)s_params.wd_ms);
    LOG_I("J8108", "limits: p[%.0f..%.0f]deg vmax=%.0fdps tmax=%.1fNm rate=%.0fdps2 | gains kd_damp=%.2f kp_pos=%.1f kd_pos=%.2f kp_v=%.3f ki_v=%.4f",
          (double)s_params.pmin_deg, (double)s_params.pmax_deg, (double)s_params.vmax_dps, (double)s_params.tmax_nm,
          (double)s_params.rate_dps2, (double)s_params.kd_damp, (double)s_params.kp_pos, (double)s_params.kd_pos,
          (double)s_params.kp_v, (double)s_params.ki_v);
    LOG_I("J8108", "SAFETY: enable via serial #EN only (no local button action). watchdog %ums -> DAMP (not disable). PB2 = UI only",
          (unsigned)s_params.wd_ms);
    LOG_I("J8108", "========================================");

    for (;;)
    {
        uint32_t t = now_ms();
        uint32_t dt = t - prev_ms;

        stack_probe_tick(t, "J8108", J8108_TASK_STACK_WORDS); /* 栈余量自报（5s 一次） */

        prev_ms = t;
        if (dt > s_dt_max_ms)
        {
            s_dt_max_ms = dt; /* 记录本窗口最大循环间隔（饿死/长阻塞会体现在这里） */
        }
        if (dt == 0u)
        {
            dt = 1u;
        }
        if (dt > 100u)
        {
            dt = 100u; /* 调度抖动/长阻塞保护：dt 上限 100ms，避免积分与速率限制算飞 */
        }

        J8108_Update();        /* 解算 + 状态 + 发布快照（供 UI/协议读） */
        j8108_link_debug(t);   /* 边沿与低频摘要 */
        j8108_req_poll(t);     /* 使能/失能/零点握手（含无 ACK 丢帧） */
        j8108_mode_poll(t, dt);/* 心跳 + 保护 → @EVT */
        j8108_frame_poll(t, dt);/* 生成 + 发送控制帧 + 帧看门狗 */

        vTaskDelay(1);
    }
}

void J8108_Task_Init(void)
{
    BaseType_t ok;

    /* ★ 必须先做 CAN 注册（注册滤波器 + 启动控制器），否则状态恒为 INIT_FAIL、
     *   既收不到反馈帧也发不出帧 —— 2026-09-18 重写时漏掉这一步导致实机 INIT FAIL。
     *   J8108_Init 内部失败会打 E 级日志（CAN init FAILED ...）。 */
    J8108_Init(&hcan1);

    s_dev = J8108_Get();
    Ctrl_ParamsDefault(&s_params);
    Ctrl_Init(&s_ctrl, &s_params);

    ok = xTaskCreate(j8108_task, "J8108", J8108_TASK_STACK_WORDS, NULL, J8108_TASK_PRIORITY, &s_task_handle);
    if (ok != pdPASS)
    {
        LOG_E("J8108", "xTaskCreate FAILED (heap/stack?) -> MOTOR CONTROL DISABLED");
    }
}

/* --------------------------- 模块自检（#ST j8108 / #ST can） --------------------------- */
uint8_t J8108_SelfTest(void)
{
    /* 非破坏性：只看"任务起没起来、设备注册没有"。是否使能/模式正常由 #STAT 反映，这里不重复判断。 */
    if (s_task_handle == NULL)
        return 3u; /* 控制任务没创建 → 电机控制整体失效（堆不足？看开机日志） */
    if (J8108_Get() == NULL)
        return 3u; /* CAN 设备未注册（J8108_Init 失败/设备表满） */
    return 1u;
}

uint8_t J8108_CanSelfTest(void)
{
    const J8108_t *d = J8108_Get();

    if (d == NULL)
        return 3u;
    switch (d->status)
    {
    case J8108_ST_READY:
        return 1u; /* 收到过反馈帧：链路已建立 */
    case J8108_ST_INIT_OK:
        return 1u; /* 控制器已启动、还没等到节点（电机没上电 ≠ 故障） */
    case J8108_ST_BUS_ERR:
        return 2u; /* 总线错误：无应答/接线/终端电阻 */
    default:
        return 3u; /* INIT_FAIL：CAN 注册或启动就失败了 */
    }
}

#else  /* !FEATURE_J8108 —— 服务模块的"显式停用桩"（设计取舍见 docs/日志_模块化改造.md M2） */
/*
 * 为什么这里不是 K3 那种"空对象"：
 *   本模块的 API 被**通用协议层**（CmdRx）和 UI 层调用。编成空对象的话，那些调用点必须
 *   逐个 #if 包住 —— 等于让协议层复制一份"电机命令表"，维护负担大于收益且容易漏。
 *   折中：**不碰任何硬件**，但每个 API 都诚实回答"我没在跑"：
 *     · 请求类  ：ReqStart 记录 → ReqResult 返回 J8108_RES_DISABLED → 协议层回 @ERR 4
 *     · 设置类  ：空操作（系统里没有电机在动）
 *     · 查询类  ：返回静态默认对象（UI/协议读取不崩）
 *   纯硬件驱动（disp/sd/can 之类，不该被通用层调用）仍按 K3 编成空对象。
 */
static Ctrl_Params_t s_p_stub;
static Ctrl_State_t s_s_stub;
static uint8_t s_stub_ready = 0u;
static volatile uint8_t s_stub_req = (uint8_t)J8108_REQ_NONE;

static void stub_ready(void)
{
    if (s_stub_ready == 0u)
    {
        Ctrl_ParamsDefault(&s_p_stub);
        Ctrl_Init(&s_s_stub, &s_p_stub);
        s_stub_ready = 1u;
    }
}

void J8108_Task_Init(void)
{
    stub_ready();
    LOG_W("J8108", "module DISABLED by feature_config.h (stub active, NO CAN traffic, NO motion)");
}

void J8108_ReqStart(J8108_Req_e r, const char *why)
{
    (void)why;
    s_stub_req = (uint8_t)r; /* 立即完成：ReqResult 会回 DISABLED，协议层据此回 @ERR 4 */
}

J8108_Res_e J8108_ReqResult(void)
{
    return (s_stub_req != (uint8_t)J8108_REQ_NONE) ? J8108_RES_DISABLED : J8108_RES_NONE;
}

void J8108_ReqClear(void) { s_stub_req = (uint8_t)J8108_REQ_NONE; }
void J8108_KeepAlive(void) { }
uint8_t J8108_IsEnabled(void) { return 0u; }
uint8_t J8108_IsSending(void) { return 0u; }
uint8_t J8108_SetMode(Ctrl_Mode_e m) { (void)m; return 0u; }
void J8108_SetPosDeg(float deg) { (void)deg; }
void J8108_SetVelDps(float dps) { (void)dps; }
void J8108_SetVelTff(float tff) { (void)tff; }
void J8108_SetTorqueNm(float nm) { (void)nm; }
void J8108_SetImp(float kp, float kd, float tff) { (void)kp; (void)kd; (void)tff; }
void J8108_SetDampKd(float kd) { (void)kd; }
void J8108_SetRawMIT(float p_rad, float v_rps, float kp, float kd, float t_nm)
{
    (void)p_rad; (void)v_rps; (void)kp; (void)kd; (void)t_nm;
}
void J8108_StopSoft(void) { }
void J8108_StopHard(void) { }
const Ctrl_State_t *J8108_State(void) { stub_ready(); return &s_s_stub; }
Ctrl_Params_t *J8108_Params(void) { stub_ready(); return &s_p_stub; }
uint32_t J8108_EventLatch(void) { return 0u; }
uint32_t J8108_LoopJitterTake(void) { return 0u; }

void J8108_DbgFrames(float *kp, float *kd)
{
    if (kp != NULL) { *kp = 0.0f; }
    if (kd != NULL) { *kd = 0.0f; }
}

uint8_t J8108_SelfTest(void)
{
    return 0u; /* 0 = 本固件未编译该模块（#ST 汇总行据此显示） */
}

uint8_t J8108_CanSelfTest(void)
{
    return 0u;
}

#endif /* FEATURE_J8108 */
