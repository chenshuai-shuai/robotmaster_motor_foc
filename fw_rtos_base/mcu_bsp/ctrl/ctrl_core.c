/*
 * ctrl_core.c - 关节外环控制核实现（纯逻辑：无 HAL / FreeRTOS / math.h 依赖）
 *
 * 单位：入参/出参均为**输出端**——位置 deg、速度 deg/s、力矩 N·m；
 *       输出帧参数换算成 rad / rad·s⁻¹（与电机反馈同口径，双编不除 8）。
 */
#include "ctrl_core.h"
#include "string.h"

#define CTRL_RAD2DEG (57.29578f)
#define CTRL_DEG2RAD (0.017453292f)

static float f_abs(float v)
{
    return (v < 0.0f) ? -v : v;
}

static float clampf(float v, float lo, float hi)
{
    if (v < lo)
        return lo;
    if (v > hi)
        return hi;
    return v;
}

/* 设定点速率限制：每秒最多变化 rate（rate<=0 → 不限速，直接跳变） */
static float slew(float cur, float target, float rate, uint32_t dt_ms)
{
    float max_d;
    float d;

    if (rate <= 0.0f)
        return target;
    max_d = rate * ((float)dt_ms * 0.001f);
    d = target - cur;
    if (d > max_d)
        return cur + max_d;
    if (d < -max_d)
        return cur - max_d;
    return target;
}

void Ctrl_ParamsDefault(Ctrl_Params_t *p)
{
    if (p == NULL)
        return;
    memset(p, 0, sizeof(*p));

    /* 增益（保守值；**实机 2026-09-18 教训**：kp_v=0.05 对这条小惯量直驱关节太大 →
     * 200Hz 下每 5ms 一步全力矩就能把关节加速上百 dps → 极限环震荡（表现为"猛抖一下"）。
     * 现在改为小比例 + 力矩斜率限制，靠 trate_nm_s 保证"每周期力矩增量"远小于目标速度变化） */
    p->kd_damp = 1.0f;
    p->kp_pos = 20.0f;
    p->kd_pos = 1.0f;
    p->kp_v = 0.005f;
    p->ki_v = 0.0005f;
    p->kp_imp = 10.0f;
    p->kd_imp = 0.5f;
    /* 判定/限幅 */
    p->inpos_deg = 1.0f;
    p->pmin_deg = -170.0f;
    p->pmax_deg = 170.0f;
    p->vmax_dps = 300.0f;
    p->tmax_nm = 7.5f;
    p->rate_dps2 = 3000.0f;
    p->trate_nm_s = 20.0f; /* 20 N·m/s → 200Hz 下每周期 ≤0.1 N·m，小惯量关节的稳定关键 */
    p->tff_nm = 0.0f;      /* 摩擦前馈默认 0（显式设定：#SET TFF <Nm>） */
    /* 保护阈值 */
    p->follow_deg = 30.0f;
    p->follow_ms = 500u;
    p->stall_nm = 8.0f;
    p->stall_dps = 10.0f;
    p->stall_ms = 1000u;
    p->temp_warn_c = 70.0f;
    p->temp_stop_c = 85.0f;
    p->wd_ms = 200u;
    /* 开关 */
    p->autodamp_on_err = 1u;
    p->setp_unlocked = 0u;
}

void Ctrl_Init(Ctrl_State_t *s, const Ctrl_Params_t *p)
{
    if (s == NULL)
        return;
    memset(s, 0, sizeof(*s));
    s->mode = CTRL_MODE_IDLE;
    s->enabled = 0u;
    s->last_cmd_ms = 0u;
    (void)p;
}

void Ctrl_SetMode(Ctrl_State_t *s, Ctrl_Mode_e m, float fb_pos_deg, float fb_vel_dps)
{
    if (s == NULL)
        return;

    s->mode = m;
    s->integ = 0.0f;
    s->t_cmd = 0.0f;         /* 力矩从 0 起（配合 trate_nm_s，防"一大步力矩"） */
    s->lim_latched = 0u;
    s->lim_clear_ms = 0u;
    s->moterr_clear_ms = 0u;
    s->follow_acc_ms = 0u;
    s->stall_acc_ms = 0u;

    switch (m)
    {
    case CTRL_MODE_SPEED:
        /* 进速度模式从"当前为 0"起步（安全：不会突然按旧目标冲） */
        s->set = 0.0f;
        s->set_applied = 0.0f;
        break;
    case CTRL_MODE_POS:
        /* 进位置模式：以**当前实际角**为起点，避免瞬间大误差跳变 */
        s->set = fb_pos_deg;
        s->set_applied = fb_pos_deg;
        s->inpos = 1u;
        break;
    case CTRL_MODE_IMP:
        s->imp_ref_deg = fb_pos_deg;
        s->set = fb_pos_deg;
        s->set_applied = fb_pos_deg;
        break;
    case CTRL_MODE_TORQUE:
        s->set = 0.0f;
        s->set_applied = 0.0f;
        s->tff = 0.0f;
        break;
    case CTRL_MODE_DAMP:
    case CTRL_MODE_IDLE:
    default:
        s->set = 0.0f;
        s->set_applied = 0.0f;
        break;
    }
    (void)fb_vel_dps;
}

void Ctrl_SetPos(Ctrl_State_t *s, float deg)
{
    if (s != NULL)
        s->set = deg;
}

void Ctrl_SetVel(Ctrl_State_t *s, float dps)
{
    if (s != NULL)
        s->set = dps;
}

void Ctrl_SetTorque(Ctrl_State_t *s, float nm)
{
    if (s != NULL)
        s->set = nm;
}

/* 速度环前馈（N·m）：由 #V 的第 3 个参数设置，**不切模式** */
void Ctrl_SetTff(Ctrl_State_t *s, float tff)
{
    if (s != NULL)
        s->tff = tff;
}

void Ctrl_SetImp(Ctrl_State_t *s, Ctrl_Params_t *p, float kp, float kd, float tff)
{
    if ((s == NULL) || (p == NULL))
        return;
    p->kp_imp = clampf(kp, 0.0f, 500.0f); /* 电机侧 Kp 上限 500 */
    p->kd_imp = clampf(kd, 0.0f, 5.0f);   /* 电机侧 Kd 上限 5 */
    s->tff = clampf(tff, -p->tmax_nm, p->tmax_nm);
    s->imp_ref_deg = s->set_applied; /* 以当前已应用设定点为参考角 */
}

void Ctrl_SetDamp(Ctrl_State_t *s, Ctrl_Params_t *p, float kd)
{
    if ((s == NULL) || (p == NULL))
        return;
    p->kd_damp = clampf(kd, 0.0f, 5.0f);
    s->mode = CTRL_MODE_DAMP;
    s->set = 0.0f;
    s->set_applied = 0.0f;
    s->integ = 0.0f;
}

void Ctrl_Release(Ctrl_State_t *s)
{
    if (s == NULL)
        return;
    s->mode = CTRL_MODE_DAMP;
    s->set = 0.0f;
    s->set_applied = 0.0f;
    s->integ = 0.0f;
    s->tff = 0.0f;
}

void Ctrl_Ping(Ctrl_State_t *s, uint32_t now_ms)
{
    if (s != NULL)
        s->last_cmd_ms = now_ms;
}

void Ctrl_Estop(Ctrl_State_t *s)
{
    if (s == NULL)
        return;
    s->estop = 1u;
    s->enabled = 0u; /* 硬急停 = 失能（由调用方发 …FD） */
    s->mode = CTRL_MODE_DAMP;
    s->set = 0.0f;
    s->set_applied = 0.0f;
    s->integ = 0.0f;
    s->ev |= CTRL_EV_ESTOP;
}

uint8_t Ctrl_TakeEvents(Ctrl_State_t *s)
{
    uint8_t e;

    if (s == NULL)
        return 0u;
    e = (uint8_t)s->ev;
    s->ev = 0u;
    return e;
}

uint8_t Ctrl_Heartbeat(Ctrl_State_t *s, const Ctrl_Params_t *p, uint32_t now_ms)
{
    if ((s == NULL) || (p == NULL) || (p->wd_ms == 0u))
        return 0u;

    if ((uint32_t)(now_ms - s->last_cmd_ms) <= (uint32_t)p->wd_ms)
        return 0u; /* 心跳正常 */

    if ((s->mode != CTRL_MODE_DAMP) && (s->mode != CTRL_MODE_IDLE))
    {
        /* 超时：退阻尼（**不失能**：本电机无抱闸，失能会自由坠落） */
        s->mode = CTRL_MODE_DAMP;
        s->set = 0.0f;
        s->set_applied = 0.0f;
        s->integ = 0.0f;
        s->ev |= CTRL_EV_TIMEOUT;
        return 1u;
    }
    return 0u;
}

uint16_t Ctrl_Protect(Ctrl_State_t *s, const Ctrl_Params_t *p, float pos_deg, float vel_dps,
                      float t_nm, float t_mos, float t_rotor, uint8_t motor_err, uint32_t dt_ms)
{
    uint16_t new_ev = 0u;

    if ((s == NULL) || (p == NULL))
        return 0u;

    /* ---- 电机报错位（双编 byte0）---- */
    if (motor_err != 0u)
    {
        s->moterr_clear_ms = 0u;
        if (s->moterr_latched == 0u) /* 边沿锁存 + 消退去抖：持续/抖动报错都只通知一次 */
        {
            s->moterr_latched = 1u;
            s->ev |= CTRL_EV_MOTERR;
            new_ev |= CTRL_EV_MOTERR;
        }
        if ((p->autodamp_on_err != 0u) && (s->mode > CTRL_MODE_DAMP))
        {
            Ctrl_Release(s);
        }
    }
    else
    {
        if (s->moterr_clear_ms < CTRL_EV_CLEAR_MS)
        {
            s->moterr_clear_ms = (uint16_t)(s->moterr_clear_ms + dt_ms);
        }
        if (s->moterr_clear_ms >= CTRL_EV_CLEAR_MS)
        {
            s->moterr_latched = 0u; /* 连续 1s 无报错：重新武装（下次再报会再通知） */
        }
    }

    /* ---- 过温 ---- */
    if ((t_mos >= p->temp_stop_c) || (t_rotor >= p->temp_stop_c))
    {
        if (s->temp_warn_latched == 0u)
        {
            s->temp_warn_latched = 1u;
            s->ev |= CTRL_EV_TEMP;
            new_ev |= CTRL_EV_TEMP;
        }
        if (s->mode > CTRL_MODE_DAMP)
        {
            Ctrl_Release(s); /* 过温停机：退阻尼 */
        }
    }
    else if ((t_mos >= p->temp_warn_c) || (t_rotor >= p->temp_warn_c))
    {
        if (s->temp_warn_latched == 0u)
        {
            s->temp_warn_latched = 1u; /* 预警只报一次（边沿锁存） */
            s->ev |= CTRL_EV_TEMP;
            new_ev |= CTRL_EV_TEMP;
        }
    }
    else if ((t_mos < (p->temp_warn_c - 5.0f)) && (t_rotor < (p->temp_warn_c - 5.0f)))
    {
        s->temp_warn_latched = 0u; /* 回落到预警线以下 5℃：重新武装（带滞回，防边界抖动） */
    }

    /* ---- 跟随误差（仅位置模式）---- */
    if (s->mode == CTRL_MODE_POS)
    {
        if (f_abs(s->set_applied - pos_deg) > p->follow_deg)
        {
            s->follow_acc_ms += dt_ms;
        }
        else
        {
            s->follow_acc_ms = 0u;
        }
        if ((p->follow_ms != 0u) && (s->follow_acc_ms >= (uint32_t)p->follow_ms))
        {
            s->follow_acc_ms = 0u;
            s->ev |= CTRL_EV_FOLLOW;
            new_ev |= CTRL_EV_FOLLOW;
            Ctrl_Release(s); /* 跟随不上：退阻尼，别硬顶 */
        }
    }
    else
    {
        s->follow_acc_ms = 0u;
    }

    /* ---- 堵转（运动模式：大力矩 + 低转速持续）---- */
    if (s->mode > CTRL_MODE_DAMP)
    {
        if ((f_abs(t_nm) > p->stall_nm) && (f_abs(vel_dps) < p->stall_dps))
        {
            s->stall_acc_ms += dt_ms;
        }
        else
        {
            s->stall_acc_ms = 0u;
        }
        if ((p->stall_ms != 0u) && (s->stall_acc_ms >= (uint32_t)p->stall_ms))
        {
            s->stall_acc_ms = 0u;
            s->ev |= CTRL_EV_STALL;
            new_ev |= CTRL_EV_STALL;
            Ctrl_Release(s);
        }
    }
    else
    {
        s->stall_acc_ms = 0u;
    }

    return new_ev;
}

/* ---- 力矩斜率限制：把"目标力矩"按 trate_nm_s 斜坡逼近，返回**实际下发**力矩 ----
 * 小惯量直驱关节的稳定关键：每周期力矩增量小 → 每周期速度增量小 → 不会一个周期冲过头 */
static float ctrl_t_slew(Ctrl_State_t *s, const Ctrl_Params_t *p, float t_des, uint32_t dt_ms, uint8_t *limited)
{
    float t_app = slew(s->t_cmd, t_des, p->trate_nm_s, dt_ms);

    *limited = (t_app != t_des) ? 1u : 0u;
    s->t_cmd = t_app;
    return t_app;
}

/* ---- 限幅事件：边沿锁存 + **消退去抖** --------------------------------------
 * 只在"未夹紧 → 夹紧"的跳变时报一次；而要重新武装，必须**连续 1s 不夹紧**。
 * 修复 2026-09-18 实机 bug：斜率限制时紧时松 → 纯边沿锁存被反复解锁 → 200Hz 刷屏 */
static void ctrl_lim_update(Ctrl_State_t *s, uint8_t clamped, uint8_t what, uint32_t dt_ms)
{
    if (clamped != 0u)
    {
        s->lim_clear_ms = 0u; /* 还在受限 → 清零"消失计时" */
        if (s->lim_latched == 0u)
        {
            s->lim_latched = 1u;
            s->lim_what = what;
            s->ev |= CTRL_EV_LIMIT;
        }
    }
    else
    {
        if (s->lim_clear_ms < CTRL_EV_CLEAR_MS)
        {
            s->lim_clear_ms = (uint16_t)(s->lim_clear_ms + dt_ms);
        }
        if (s->lim_clear_ms >= CTRL_EV_CLEAR_MS)
        {
            s->lim_latched = 0u; /* 连续 1s 恢复正常 → 下次受限才算新事件 */
            if (s->lim_what == what)
            {
                s->lim_what = CTRL_LIM_NONE;
            }
        }
    }
}

void Ctrl_Step(Ctrl_State_t *s, const Ctrl_Params_t *p, float fb_pos_deg, float fb_vel_dps,
               uint32_t dt_ms, Ctrl_Frame_t *out)
{
    float tgt;

    if ((s == NULL) || (p == NULL) || (out == NULL))
        return;

    out->p_rad = 0.0f;
    out->v_rps = 0.0f;
    out->kp = 0.0f;
    out->kd = 0.0f;
    out->t_nm = 0.0f;
    out->send = ((s->enabled != 0u) && (s->mode != CTRL_MODE_IDLE)) ? 1u : 0u;
    if (out->send == 0u)
        return;

    switch (s->mode)
    {
    case CTRL_MODE_DAMP:
        /* 纯阻尼：T = -Kd·v（软住、可拖动、不发力） */
        out->kd = p->kd_damp;
        s->t_cmd = 0.0f; /* 下个力矩模式从 0 起爬 */
        break;

    case CTRL_MODE_SPEED:
    {
        float e;
        float t;
        float tcl;
        float t_app;
        uint8_t lim_t = 0u;
        uint8_t clamped = 0u;

        tgt = clampf(s->set, -p->vmax_dps, p->vmax_dps);
        if (tgt != s->set)
        {
            clamped = 1u; /* 设定点被限速夹紧 */
        }
        s->set_applied = slew(s->set_applied, tgt, p->rate_dps2, dt_ms);
        e = s->set_applied - fb_vel_dps;
        /* tff = 显式前馈（#V 第 3 参，不随方向变）；tff_nm = **库仑摩擦前馈**（按设定点方向取号） */
        t = (p->kp_v * e) + s->integ + s->tff + ((s->set_applied >= 0.0f) ? p->tff_nm : -p->tff_nm);
        tcl = clampf(t, -p->tmax_nm, p->tmax_nm);
        if (tcl != t)
        {
            clamped = 1u; /* 力矩被限幅夹紧 */
        }
        t_app = ctrl_t_slew(s, p, tcl, dt_ms, &lim_t); /* ★ 力矩斜率限制 */
        if (lim_t != 0u)
        {
            clamped = 1u;
        }
        /* 抗积分饱和：只有"完全没被限制"（限幅与斜率都没触发）才积分 */
        if ((tcl == t) && (lim_t == 0u))
        {
            s->integ += p->ki_v * e * ((float)dt_ms * 0.001f);
            s->integ = clampf(s->integ, -p->tmax_nm, p->tmax_nm);
        }
        out->t_nm = t_app;
        out->kd = p->kd_damp; /* 轻微阻尼，压振荡 */
        ctrl_lim_update(s, clamped,
                        (tcl != t) ? CTRL_LIM_TORQUE : ((lim_t != 0u) ? CTRL_LIM_TRATE : CTRL_LIM_SETPOINT), dt_ms);
        break;
    }

    case CTRL_MODE_POS:
        tgt = clampf(s->set, p->pmin_deg, p->pmax_deg);
        s->set_applied = slew(s->set_applied, tgt, p->rate_dps2, dt_ms);
        out->p_rad = s->set_applied * CTRL_DEG2RAD;
        out->kp = p->kp_pos;
        out->kd = p->kd_pos; /* 文档警告：Kp≠0 必须配 Kd，否则震荡/失控 */
        s->inpos = (f_abs(s->set_applied - fb_pos_deg) <= p->inpos_deg) ? 1u : 0u;
        s->t_cmd = 0.0f;                                                            /* 位置模式不由我们出力矩 */
        ctrl_lim_update(s, (tgt != s->set) ? 1u : 0u, CTRL_LIM_SETPOINT, dt_ms);
        break;

    case CTRL_MODE_TORQUE:
    {
        float t_des = clampf(s->set, -p->tmax_nm, p->tmax_nm);
        uint8_t lim_t = 0u;

        out->t_nm = ctrl_t_slew(s, p, t_des, dt_ms, &lim_t);
        ctrl_lim_update(s, ((t_des != s->set) || (lim_t != 0u)) ? 1u : 0u,
                        (t_des != s->set) ? CTRL_LIM_TORQUE : CTRL_LIM_TRATE, dt_ms);
        break;
    }

    case CTRL_MODE_IMP:
    {
        float t_des = clampf(s->tff, -p->tmax_nm, p->tmax_nm);
        uint8_t lim_t = 0u;

        out->p_rad = s->imp_ref_deg * CTRL_DEG2RAD;
        out->kp = p->kp_imp;
        out->kd = p->kd_imp;
        out->t_nm = ctrl_t_slew(s, p, t_des, dt_ms, &lim_t);
        ctrl_lim_update(s, ((t_des != s->tff) || (lim_t != 0u)) ? 1u : 0u,
                        (t_des != s->tff) ? CTRL_LIM_TORQUE : CTRL_LIM_TRATE, dt_ms);
        break;
    }

    case CTRL_MODE_IDLE:
    default:
        out->send = 0u;
        break;
    }
}

const char *Ctrl_LimitWhat(const Ctrl_State_t *s)
{
    if (s == NULL)
    {
        return "none";
    }
    if (s->lim_what == CTRL_LIM_SETPOINT)
    {
        return "setpoint";
    }
    if (s->lim_what == CTRL_LIM_TORQUE)
    {
        return "torque";
    }
    if (s->lim_what == CTRL_LIM_TRATE)
    {
        return "trate"; /* 力矩斜率限制（TRATE）—— 不是撞上限幅，只是 ramp 被限速 */
    }
    return "none";
}

const char *Ctrl_ModeStr(Ctrl_Mode_e m)
{
    switch (m)
    {
    case CTRL_MODE_IDLE:
        return "IDLE";
    case CTRL_MODE_DAMP:
        return "DAMP";
    case CTRL_MODE_SPEED:
        return "SPEED";
    case CTRL_MODE_POS:
        return "POS";
    case CTRL_MODE_TORQUE:
        return "TORQUE";
    case CTRL_MODE_IMP:
        return "IMP";
    default:
        return "?";
    }
}
