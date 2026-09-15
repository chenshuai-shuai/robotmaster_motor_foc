/*
 * J8108_Task.c - 8108 关节电机任务（A 板 CAN1，1Mbps）
 *
 * 目标：用 RoboMaster A 板控制 8108 关节电机（STACKFORCE 单编 24V）
 *   ① CAN 链路（收到 0x781 反馈帧） ② 使能（RGB 红→绿） ③ 控制+反馈闭环（能转、数字对）
 *
 * 运行模式（宏切换，见 feature_config.h 与本文件 J8108_AUTO_DEMO）：
 *   - 默认 J8108_AUTO_DEMO=0 → **只监听，不发任何帧**（M1/M2 阶段，电机绝对安全）
 *   - =1 → 上电自动演示序列：使能 → 校验反馈 → 阻尼 → 慢转正/反 → 自动失能
 *
 * 日志约定：全部英文/ASCII（串口终端下中文会乱码）。
 * 开发期详日志（FEATURE_VERBOSE_LOG=1）：前 N 帧反馈打印**原始字节 + 解算值**（校验解析用），
 *   并打印链路 UP/DOWN 边沿；之后每 10s 一条摘要。
 *
 * 接线（通电前核对）：A 板 CAN1 口 CAN_H/CAN_L(/GND) ↔ 电机 XT30(2+2) 的 CAN 两针（H/L 勿反）；
 *   电机 24V 独立供电；电机端 120Ω 开关开（A 板自带 R80 120Ω）；电机默认 CANID=0x01；先单机挂总线。
 */
#include "J8108_Task.h"

#include "FreeRTOS.h"
#include "task.h"
#include "bsp_log.h"
#include "motor_8108.h"
#include "j8108_action.h"
#include "bsp_key.h"   /* 长按事件（Key_EventGroup / KEY_BIT_HOLD_RELEASE） */
#include "Oled_Task.h" /* 读当前光标行（决定长按作用在哪一行） */
#include "version.h"
#include "main.h"
#include "feature_config.h"

/* ---- 测试参数（按需修改） ---- */
#define J8108_STARTUP_DELAY_MS 3000u /* 上电静置 */
#define J8108_CHECK_MS 1500u         /* 每次等反馈的窗口 */
#define J8108_ENABLE_RETRY_MAX 3u    /* 使能尝试次数（含首次） */
#define J8108_DAMP_MS 3000u          /* 阻尼保持时长 */
#define J8108_SPIN_MS 3000u          /* 单向慢转时长 */
#define J8108_DAMP2_MS 2000u
#define J8108_SPIN_SPEED_RADS 0.6f /* 慢转速度（输出端 rad/s，约 5.7 RPM） */
#define J8108_CTRL_PERIOD_MS 5u    /* MIT 帧周期（200Hz） */
#define J8108_PRINT_PERIOD_MS 500u /* 反馈打印周期 */
#define J8108_DAMP_KD 1.0f         /* 阻尼系数（首动作，防乱动） */
#define J8108_HOLD_ON_ENABLE 1u    /* 1=使能确认后自动进入"阻尼保持"(HOLD)：周期发 MIT 帧。
                                    *   **实测依据（2026-09-15）**：本电机反馈帧是"响应式心跳"——只发 0xFC 使能
                                    *   后它绿灯亮但**不回传任何帧**；必须持续发 MIT 帧才有反馈（台架演示程序
                                    *   正是 200Hz 发帧才收到反馈）。HOLD 即"读回参数"的通道，也是安全阻尼。
                                    *   0=使能后完全不发帧（只用来验证"是否真的不发帧就没反馈"）*/
#define J8108_HOLD_WD_MS 20u       /* HOLD 看门狗轮询周期 */
#define J8108_HOLD_TX_STUCK_MS 100u /* **连续占用**超过此值才判"无 ACK"（实测踩坑：最初用"邮箱非全空"判 → 把正在发送的 130µs 误判成无 ACK，把 HOLD 反复掐断）*/
#define J8108_HOLD_FB_LOST_MS 500u /* HOLD 模式下反馈超时 → 退出并尝试恢复 */
#define J8108_HOLD_RETRY_MS 500u   /* 自动恢复重试间隔基数（第 n 次失败退避 n×基数）*/
#define J8108_HOLD_RETRY_MAX 4u    /* 连续失败上限：超过则放弃并提示（防无限刷屏/重传；用户再按一次即可重来）*/

/* ---- 运行模式 ---- */
#define J8108_AUTO_DEMO 0u         /* 1=上电自动演示；0=只监听，不发任何帧 */
#define J8108_LISTEN_LOG_MS 10000u /* 仅监听模式：状态摘要周期 */
#define J8108_FB_LOG_MS 1000u      /* 反馈流日志周期（0=关闭）：接入电机后 1Hz 打一条解算值，
                                    * 便于手拧输出轴时在终端盯数字（验证解析用；噪声仅 1 行/秒） */
#define J8108_FB_TIMEOUT_MS 200u   /* 反馈超时 → 判链路断 */
#define J8108_VERBOSE_FRAMES 10u   /* 详日志：前 N 帧逐帧打印（含原始字节） */

#define J8108_TASK_PRIORITY 5u
#define J8108_TASK_STACK_WORDS 640u

typedef enum
{
    PH_IDLE = 0,
    PH_ENABLE,
    PH_CHECK,
    PH_DAMP,
    PH_SPIN_FWD,
    PH_SPIN_REV,
    PH_DAMP2,
    PH_DONE
} J8108_Phase_e;

static TaskHandle_t s_task_handle;
static J8108_t *s_dev;
static J8108_Phase_e s_phase = PH_IDLE;
static uint32_t s_phase_start_ms = 0;
static uint32_t s_try_cnt = 0;
static uint32_t s_mark_count = 0; /* 发使能时的反馈计数基线 */
static uint8_t s_link_ok = 0;     /* 链路状态（边沿日志用） */
static uint8_t s_status_dbg = 0xFFu; /* 状态边沿日志用（0xFF=未初始化） */
static uint32_t s_last_rx_dbg = 0;

static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static void phase_enter(J8108_Phase_e ph)
{
    s_phase = ph;
    s_phase_start_ms = now_ms();
}

static void print_feedback(void)
{
    LOG_I("J8108", "fb pos=%.3frad(motor)/%.1fdeg(out) vel=%.2frad/s T=%.3fNm Tm=%.1fC Tr=%.1fC rx=%lu",
          (double)s_dev->fb.pos, (double)(s_dev->fb.pos / 8.0f * 57.2958f),
          (double)s_dev->fb.vel, (double)s_dev->fb.torque,
          (double)s_dev->fb.t_mos, (double)s_dev->fb.t_rotor,
          (unsigned long)s_dev->fb.rx_count);
}

/* 链路 UP/DOWN 边沿 + （详日志）前 N 帧原始字节 */
static void j8108_link_debug(uint32_t t)
{
    uint8_t now_ok = 0u;

    if (s_dev->fb.rx_count > 0u)
    {
        now_ok = ((t - s_dev->fb.last_rx_ms) < J8108_FB_TIMEOUT_MS) ? 1u : 0u;
    }

    if (now_ok != s_link_ok)
    {
        s_link_ok = now_ok;
        if (now_ok != 0u)
        {
            LOG_I("J8108", "CAN feedback link UP (rx=%lu) -> motor is powered AND enabled",
                  (unsigned long)s_dev->fb.rx_count);
        }
        else
        {
            LOG_W("J8108", "CAN feedback link LOST last_dt=%lums (motor disabled/power off/wiring)",
                  (unsigned long)(t - s_dev->fb.last_rx_ms));
        }
    }

    /* 状态变化才打日志（屏上四态同步）：INIT FAIL / INIT OK(WAITING) / READY / BUS ERR */
    if (s_dev->status != s_status_dbg)
    {
        s_status_dbg = s_dev->status;
        switch (s_status_dbg)
        {
        case J8108_ST_INIT_FAIL:
            LOG_E("J8108", "CAN status -> INIT FAIL (no retry by design): check CAN peripheral / device table");
            break;
        case J8108_ST_INIT_OK:
            LOG_I("J8108", "CAN status -> INIT OK / WAITING for node (controller started, no frame on bus yet)");
            break;
        case J8108_ST_READY:
            LOG_I("J8108", "CAN status -> READY (feedback frames arriving): channel established, safe to send frames");
            break;
        case J8108_ST_BUS_ERR:
            LOG_W("J8108", "CAN status -> BUS ERR (HAL err=0x%04lX): check ACK/wiring/120R", (unsigned long)s_dev->bus_err);
            break;
        default:
            break;
        }
    }

#if FEATURE_VERBOSE_LOG
    if ((s_dev->fb.rx_count != s_last_rx_dbg) && (s_dev->fb.rx_count <= J8108_VERBOSE_FRAMES))
    {
        const uint8_t *d = s_dev->fb.raw;
        s_last_rx_dbg = s_dev->fb.rx_count;
        LOG_D("J8108", "fb#%lu raw %02X %02X %02X %02X %02X %02X %02X %02X | pos=%.3f vel=%.2f T=%.3f Tm=%.1f Tr=%.1f",
              (unsigned long)s_dev->fb.rx_count, d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7],
              (double)s_dev->fb.pos, (double)s_dev->fb.vel, (double)s_dev->fb.torque,
              (double)s_dev->fb.t_mos, (double)s_dev->fb.t_rotor);
    }
#endif
}

/* ========================= M3：单键动作层（L2 冷却 / L3.5 ACK / L4 反馈确认） ========================= */
static uint32_t s_last_action_ms = 0u;                 /* L2 冷却基准 */
static uint32_t s_act_result_ms = 0u;                  /* 结果时间戳（UI 判"最近 2.5s 内"） */
static J8108_ActionResult_e s_act_result = ACTR_NONE;  /* 最近动作结果（屏上显示） */
static J8108_Action_t s_pending = ACT_DO_NOTHING;      /* 已发帧、待确认的动作 */
static uint32_t s_mark_rx = 0u;                        /* 发帧时的反馈计数基线 */
static uint32_t s_ack_dl_ms = 0u;                      /* L3.5 截止时刻（0=已检查完） */
static uint32_t s_confirm_dl_ms = 0u;                  /* L4 截止时刻 */
/* 前置声明：下面 HOLD 段要用 M3 段里定义的两个静态辅助函数（定义在其后） */
static CAN_HandleTypeDef *j8108_can(void);
static void act_result_set(J8108_ActionResult_e r, uint32_t t);

static uint8_t s_hold = 0u;                            /* HOLD（阻尼保持）模式开关 */
static uint32_t s_hold_tx_ms = 0u;                     /* 上次发 MIT 帧时刻 */
static uint32_t s_hold_wd_ms = 0u;                     /* 看门狗下次检查时刻 */
static uint8_t s_hold_want = 0u;                       /* 用户意图：保持控制流（使能成功=1；失能/放弃=0）*/
static uint32_t s_hold_retry_ms = 0u;                  /* 下次自动恢复时刻 */
static uint8_t s_hold_fail = 0u;                       /* 连续失败计数（有进展即清零）*/
static uint32_t s_hold_rx_seen = 0u;                   /* 上次看到的反馈计数 */
static uint8_t s_ack_checked = 0u;                     /* L3.5 单次检查标志（防每轮重复进入 HOLD）*/
static uint8_t s_tx_busy = 0u;                         /* TX 邮箱连续占用计时 */
static uint32_t s_tx_busy_ms = 0u;

/* TX 邮箱是否"**连续**占用"超过 limit_ms —— 只有持续占用才算无人 ACK；
 * 瞬时占用（该帧正在发送，~130µs@1Mbps）必须放过，否则会把正常运行判成故障。 */
static uint8_t j8108_tx_stuck(uint32_t t, uint32_t limit_ms)
{
    CAN_HandleTypeDef *h = j8108_can();

    if (h == NULL)
    {
        return 0u;
    }
    if (HAL_CAN_GetTxMailboxesFreeLevel(h) < 3u)
    {
        if (s_tx_busy == 0u)
        {
            s_tx_busy = 1u;
            s_tx_busy_ms = t;
        }
        return ((t - s_tx_busy_ms) >= limit_ms) ? 1u : 0u;
    }
    s_tx_busy = 0u;
    return 0u;
}

static void hold_enter(uint32_t t, const char *why)
{
    if (s_hold != 0u)
    {
        return;
    }
    s_hold = 1u;
    s_hold_tx_ms = 0u; /* 下一轮立即发第一帧 */
    s_hold_wd_ms = t + J8108_HOLD_WD_MS;
    LOG_I("J8108", "HOLD mode ON (%s): sending MIT damping frames v=0 Kp=0 Kd=%.1f every %ums"
                   " -> feedback (heartbeat) should now stream at the same rate",
          why, (double)J8108_DAMP_KD, J8108_CTRL_PERIOD_MS);
}

static void hold_exit(uint32_t t, const char *why)
{
    if (s_hold == 0u)
    {
        return;
    }
    s_hold = 0u;
    (void)t;
    LOG_W("J8108", "HOLD mode OFF (%s): MIT frames stopped", why);
}

uint8_t J8108_IsHoldMode(void)
{
    return s_hold;
}

/* HOLD 模式：周期发 MIT 阻尼帧 + 看门狗（连续无 ACK / 反馈丢失 → 停发 + 退避自动恢复） */
static void j8108_hold_poll(uint32_t t)
{
    CAN_HandleTypeDef *h;

    if (s_hold == 0u)
    {
        /* 有保持意图但当前没在发（被误判掐断 / 反馈中断 / 电机刚回来）→ 退避自动恢复 */
        if ((s_hold_want != 0u) && (s_hold_fail < J8108_HOLD_RETRY_MAX) &&
            ((int32_t)(t - s_hold_retry_ms) >= 0))
        {
            hold_enter(t, "auto-resume");
        }
        return;
    }

    h = j8108_can();
    if (h == NULL)
    {
        hold_exit(t, "no CAN handle");
        return;
    }

    /* 周期发纯阻尼帧：邮箱没有空位就先不发（避免 SendMIT 内部自旋），交看门狗判死 */
    if (((t - s_hold_tx_ms) >= J8108_CTRL_PERIOD_MS) && (HAL_CAN_GetTxMailboxesFreeLevel(h) > 0u))
    {
        s_hold_tx_ms = t;
        J8108_SendMIT(0.0f, 0.0f, 0.0f, J8108_DAMP_KD, 0.0f); /* 纯阻尼：torque = -Kd·v */
    }

    /* 看门狗 */
    if ((int32_t)(t - s_hold_wd_ms) >= 0)
    {
        s_hold_wd_ms = t + J8108_HOLD_WD_MS;

        if (j8108_tx_stuck(t, J8108_HOLD_TX_STUCK_MS) != 0u)
        {
            (void)HAL_CAN_AbortTxRequest(h, CAN_TX_MAILBOX0 | CAN_TX_MAILBOX1 | CAN_TX_MAILBOX2);
            s_hold_fail++;
            s_hold_retry_ms = t + (uint32_t)J8108_HOLD_RETRY_MS * (uint32_t)s_hold_fail;
            act_result_set(ACTR_EN_NO_ACK, t);
            hold_exit(t, "no ACK");
            if (s_hold_fail >= J8108_HOLD_RETRY_MAX)
            {
                s_hold_want = 0u;
                LOG_E("J8108", "HOLD watchdog: no ACK %u times -> give up pinging (motor gone? check 24V/CAN_H-L/120R/CANID); press row0 to enable again",
                      (unsigned)J8108_HOLD_RETRY_MAX);
            }
            else
            {
                LOG_W("J8108", "HOLD watchdog: TX not ACKed (mailbox stuck) -> abort + retry in %ums (%u/%u)",
                      (unsigned)(J8108_HOLD_RETRY_MS * s_hold_fail), (unsigned)s_hold_fail, (unsigned)J8108_HOLD_RETRY_MAX);
            }
        }
        else if ((s_dev->fb.rx_count > 0u) && ((t - s_dev->fb.last_rx_ms) > J8108_HOLD_FB_LOST_MS))
        {
            s_hold_fail++;
            s_hold_retry_ms = t + (uint32_t)J8108_HOLD_RETRY_MS * (uint32_t)s_hold_fail;
            act_result_set(ACTR_EN_NO_FB, t);
            hold_exit(t, "feedback lost");
            if (s_hold_fail >= J8108_HOLD_RETRY_MAX)
            {
                s_hold_want = 0u;
                LOG_E("J8108", "HOLD watchdog: no feedback %u times -> stopped pinging (motor disabled/powered off?); press row0 to enable again",
                      (unsigned)J8108_HOLD_RETRY_MAX);
            }
            else
            {
                LOG_W("J8108", "HOLD watchdog: no feedback for %ums -> retry in %ums (%u/%u)",
                      (unsigned)J8108_HOLD_FB_LOST_MS, (unsigned)(J8108_HOLD_RETRY_MS * s_hold_fail),
                      (unsigned)s_hold_fail, (unsigned)J8108_HOLD_RETRY_MAX);
            }
        }
        else if (s_dev->fb.rx_count != s_hold_rx_seen)
        {
            s_hold_rx_seen = s_dev->fb.rx_count; /* 有进展 → 清零失败计数 */
            if (s_hold_fail != 0u)
            {
                LOG_I("J8108", "HOLD: link healthy again (rx=%lu) -> failure counter reset",
                      (unsigned long)s_dev->fb.rx_count);
                s_hold_fail = 0u;
            }
        }
    }
}

static CAN_HandleTypeDef *j8108_can(void)
{
    return (s_dev->can_instance != NULL) ? s_dev->can_instance->can_handle : NULL;
}

static void act_result_set(J8108_ActionResult_e r, uint32_t t)
{
    s_act_result = r;
    s_act_result_ms = t;
}

static void j8108_action_poll(uint32_t t)
{
    EventBits_t bits;
    KeyCore_Msg_t msg;
    J8108_Action_t act;
    uint8_t cooldown_ok;

    /* ---- L3.5：发后 20ms 检查帧是否被任何节点 ACK（TX 邮箱是否已清空）----
       本板 CAN1 配置 AutoRetransmission=ENABLE：无人应答时帧会**无限重传**，
       进而把控制器推进总线错误 → 必须主动丢弃。 */
    if ((s_pending != ACT_DO_NOTHING) && (s_ack_checked == 0u) && ((int32_t)(t - s_ack_dl_ms) >= 0))
    {
        CAN_HandleTypeDef *h = j8108_can();

        s_ack_checked = 1u; /* 单次检查：否则每轮都会重复触发（实测踩过，导致 HOLD 反复重进）*/
        if (j8108_tx_stuck(t, J8108_ACTION_ACK_MS) != 0u)
        {
            if (h != NULL)
            {
                (void)HAL_CAN_AbortTxRequest(h, CAN_TX_MAILBOX0 | CAN_TX_MAILBOX1 | CAN_TX_MAILBOX2);
            }
            act_result_set(ACTR_EN_NO_ACK, t);
            LOG_W("J8108", "no ACK in %ums -> TX aborted (no node answered). check: motor 24V / CAN_H-L / 120R / CANID=0x%02X",
                  J8108_ACTION_ACK_MS, J8108_CAN_ID);
            s_pending = ACT_DO_NOTHING;
        }
        else if ((s_pending == ACT_ENABLE) && (J8108_HOLD_ON_ENABLE != 0u))
        {
            /* 使能帧被应答 → 进入 HOLD：本电机反馈是"响应式心跳"，不发控制帧就不回传；
               同时以"纯阻尼"接管输出（torque=-Kd·v），避免使能后无指令的未知状态 */
            s_hold_want = 1u;
            s_hold_fail = 0u;
            hold_enter(t, "enable ACKed");
        }
    }

    /* ---- L4：发后 300ms 看反馈是否按预期变化（使能→出现反馈；失能→反馈停止）---- */
    if ((s_pending != ACT_DO_NOTHING) && (s_ack_dl_ms == 0u) && ((int32_t)(t - s_confirm_dl_ms) >= 0))
    {
        uint32_t rx = s_dev->fb.rx_count;

        if (s_pending == ACT_ENABLE)
        {
            if (rx > s_mark_rx)
            {
                act_result_set(ACTR_EN_OK, t);
                LOG_I("J8108", "EN confirmed: feedback +%lu frames -> motor enabled, status now %s",
                      (unsigned long)(rx - s_mark_rx), J8108_StatusStr(s_dev->status));
            }
            else
            {
                act_result_set(ACTR_EN_NO_FB, t);
                LOG_W("J8108", "EN sent but NO feedback within %ums -> check motor 24V/CANID/wiring; status=%s",
                      J8108_ACTION_CONFIRM_MS, J8108_StatusStr(s_dev->status));
            }
        }
        else if (rx == s_mark_rx)
        {
            act_result_set(ACTR_DIS_OK, t);
            LOG_I("J8108", "DIS confirmed: feedback stopped -> motor disabled");
        }
        else
        {
            act_result_set(ACTR_DIS_STILL_FB, t);
            LOG_W("J8108", "DIS sent but feedback still arriving (+%lu) -> frame may have been dropped",
                  (unsigned long)(rx - s_mark_rx));
        }
        s_pending = ACT_DO_NOTHING;
    }

    /* ---- 消费"长按松手"事件（本任务持有 HOLD_RELEASE/STUCK 位；UI 只消费 CLICK/DOUBLE）---- */
    if (Key_EventGroup() == NULL)
    {
        return;
    }
    bits = xEventGroupWaitBits(Key_EventGroup(), KEY_BIT_HOLD_RELEASE | KEY_BIT_STUCK, pdTRUE, pdFALSE, 0);
    if (bits == 0u)
    {
        return;
    }
    if ((bits & KEY_BIT_STUCK) != 0U)
    {
        LOG_E("J8108", "key STUCK -> action ignored (check PB2 hardware)");
        return;
    }

    Key_GetLastMsg(&msg, NULL);
    cooldown_ok = ((t - s_last_action_ms) >= J8108_ACTION_COOLDOWN_MS) ? 1u : 0u;
    act = J8108_ActionDecide(Oled_UiGetCursorRow(), msg.hold_ms, cooldown_ok, s_dev->status);

    switch (act)
    {
    case ACT_DO_NOTHING:
        LOG_D("J8108", "hold %ums on a read-only row -> no action (silent by design)", (unsigned)msg.hold_ms);
        break;
    case ACT_REJECT_SHORT:
        act_result_set(ACTR_SHORT, t);
        LOG_I("J8108", "hold %ums < %ums -> not triggered", (unsigned)msg.hold_ms, J8108_ACTION_HOLD_MS);
        break;
    case ACT_REJECT_COOLDOWN:
        act_result_set(ACTR_COOLDOWN, t);
        LOG_W("J8108", "action rejected: cooldown %ums not elapsed", J8108_ACTION_COOLDOWN_MS);
        break;
    case ACT_REJECT_CAN:
        act_result_set(ACTR_CAN_NOT_READY, t);
        LOG_W("J8108", "action rejected: CAN status=%s (init fail or bus error)", J8108_StatusStr(s_dev->status));
        break;
    case ACT_ENABLE:
    case ACT_DISABLE:
        if (act == ACT_DISABLE)
        {
            s_hold_want = 0u; /* 用户要失能：不再保持控制流 */
            s_hold_fail = 0u;
            hold_exit(t, "disable requested"); /* 先停发控制帧，再发失能帧 */
        }
        J8108_SendCmd((act == ACT_ENABLE) ? J8108_CMD_ENABLE : J8108_CMD_DISABLE);
        s_pending = act;
        s_ack_checked = 0u;
        s_mark_rx = s_dev->fb.rx_count;
        s_last_action_ms = t;
        s_ack_dl_ms = t + J8108_ACTION_ACK_MS;
        s_confirm_dl_ms = t + J8108_ACTION_CONFIRM_MS;
        act_result_set((act == ACT_ENABLE) ? ACTR_EN_SENT : ACTR_DIS_SENT, t);
        LOG_I("J8108", "-> %s frame sent (hold %ums, ID 0x%03X); waiting ACK(%ums) + feedback(%ums)",
              (act == ACT_ENABLE) ? "ENABLE" : "DISABLE", (unsigned)msg.hold_ms, J8108_CMD_ID,
              J8108_ACTION_ACK_MS, J8108_ACTION_CONFIRM_MS);
        break;
    default:
        break;
    }
}

J8108_ActionResult_e J8108_LastAction(uint32_t *ms_age)
{
    if (ms_age != NULL)
    {
        *ms_age = (uint32_t)(now_ms() - s_act_result_ms);
    }
    return s_act_result;
}

static void j8108_task(void *arg)
{
    uint32_t last_ctrl_ms = 0, last_print_ms = 0, last_log_ms = 0, last_fb_log_ms = 0;

    (void)arg;

    /* 注册 CAN 实例（0x781 反馈 → 回调） */
    s_dev = J8108_Get();
    J8108_Init(&hcan1);

    LOG_I("J8108", "========================================");
    LOG_I("J8108", "8108 joint motor task | FW %s | build %s %s", FW_VERSION_STR, FW_BUILD_DATE, FW_BUILD_TIME);
    LOG_I("J8108", "CAN1 1Mbps | motor CANID=0x%02X | CMD ID=0x%03X | MIT ID=0x%03X | FB ID=0x%03X",
          J8108_CAN_ID, J8108_CMD_ID, J8108_MIT_ID, J8108_FB_ID);
    LOG_I("J8108", "wiring check: CAN_H/L not swapped, motor 24V on, 120R switch ON");
    LOG_I("J8108", "========================================");

    /* ---- 只监听模式（不发任何帧，电机安全） ---- */
    if (J8108_AUTO_DEMO == 0u)
    {
        LOG_I("J8108", "MODE = MONITOR + KEY ACTION: no automatic TX. Only a %ums long-press on row0 sends enable/disable",
              J8108_ACTION_HOLD_MS);
        while (1)
        {
            uint32_t t;

            J8108_Update();
            t = now_ms();
            j8108_link_debug(t);
            j8108_action_poll(t);  /* M3：长按 → 动作（含 L2/L3.5/L4） */
            j8108_hold_poll(t);    /* M4-a：HOLD 阻尼保持（周期 MIT 帧 = 让电机回传反馈的"心跳"）*/

            if ((t - last_log_ms) >= J8108_LISTEN_LOG_MS)
            {
                last_log_ms = t;
                /* 每 10s 一条状态摘要（低频；高频基础检测一律不打，用户要求"只打事件"）。
                   **只有数据新鲜时才报数值**：链路断了坚决不打印旧数值（用户明确要求：
                   "根本就没有新数据包进来，一直打印旧数据包有啥用"）*/
                if (s_dev->fb.rx_count == 0u)
                {
                    LOG_I("J8108", "status=%s | rx=0 tx=%lu | no frame on bus (motor not connected/enabled) | waiting",
                          J8108_StatusStr(s_dev->status), (unsigned long)s_dev->fb.tx_cnt);
                }
                else if ((t - s_dev->fb.last_rx_ms) < J8108_FB_TIMEOUT_MS)
                {
                    LOG_I("J8108", "status=%s | rx=%lu tx=%lu last_dt=%lums pos=%.2frad vel=%.2frad/s",
                          J8108_StatusStr(s_dev->status), (unsigned long)s_dev->fb.rx_count,
                          (unsigned long)s_dev->fb.tx_cnt, (unsigned long)(t - s_dev->fb.last_rx_ms),
                          (double)s_dev->fb.pos, (double)s_dev->fb.vel);
                }
                else
                {
                    LOG_I("J8108", "status=%s | rx=%lu tx=%lu | link DOWN: no new frame for %lums (stale values not printed on purpose)",
                          J8108_StatusStr(s_dev->status), (unsigned long)s_dev->fb.rx_count,
                          (unsigned long)s_dev->fb.tx_cnt, (unsigned long)(t - s_dev->fb.last_rx_ms));
                }
            }

#if J8108_FB_LOG_MS > 0u
            /* 反馈流日志（M2 只读验证）：仅在**数据新鲜**时打印（>200ms 视为链路断：只靠 LOST 边沿 + 10s 摘要报，
               不重复打印同一旧帧 —— 用户明确要求）*/
            if ((s_dev->fb.rx_count > 0u) && ((t - s_dev->fb.last_rx_ms) < J8108_FB_TIMEOUT_MS) &&
                ((t - last_fb_log_ms) >= J8108_FB_LOG_MS))
            {
                last_fb_log_ms = t;
                LOG_I("J8108", "fb stream: pos=%.2frad(%.1fdeg) vel=%.2frad/s(%.0fRPM) T=%.3fNm Tm=%.1fC Tr=%.1fC age=%lums",
                      (double)s_dev->fb.pos, (double)(s_dev->fb.pos / J8108_GEAR_RATIO * 57.29578f),
                      (double)s_dev->fb.vel, (double)(s_dev->fb.vel * 9.54930f),
                      (double)s_dev->fb.torque, (double)s_dev->fb.t_mos, (double)s_dev->fb.t_rotor,
                      (unsigned long)(t - s_dev->fb.last_rx_ms));
            }
#endif

            vTaskDelay(1);
        }
    }

    phase_enter(PH_IDLE);

    while (1)
    {
        uint32_t t = now_ms();
        uint32_t elapsed = t - s_phase_start_ms;

        J8108_Update(); /* raw → 物理量 */
        j8108_link_debug(t);

        /* ---- 200Hz 控制帧 ---- */
        if (t - last_ctrl_ms >= J8108_CTRL_PERIOD_MS)
        {
            last_ctrl_ms = t;
            switch (s_phase)
            {
            case PH_ENABLE:
            case PH_CHECK:
            case PH_DAMP:
            case PH_DAMP2:
                J8108_SendMIT(0.0f, 0.0f, 0.0f, J8108_DAMP_KD, 0.0f); /* damping hold */
                break;
            case PH_SPIN_FWD:
                J8108_SendMIT(0.0f, +J8108_SPIN_SPEED_RADS, 0.0f, J8108_DAMP_KD, 0.0f);
                break;
            case PH_SPIN_REV:
                J8108_SendMIT(0.0f, -J8108_SPIN_SPEED_RADS, 0.0f, J8108_DAMP_KD, 0.0f);
                break;
            default:
                break;
            }
        }

        /* ---- 阶段推进 ---- */
        switch (s_phase)
        {
        case PH_IDLE:
            if (elapsed >= J8108_STARTUP_DELAY_MS)
            {
                J8108_SendCmd(J8108_CMD_ENABLE);
                s_mark_count = s_dev->fb.rx_count;
                s_try_cnt = 1;
                LOG_I("J8108", "-> sent ENABLE frame FF FF FF FF FF FF FF FC (ID 0x%03X); watch RGB red->green", J8108_CMD_ID);
                phase_enter(PH_ENABLE);
            }
            break;

        case PH_ENABLE:
        case PH_CHECK:
            if (elapsed >= J8108_CHECK_MS)
            {
                if (s_dev->fb.rx_count > s_mark_count)
                {
                    LOG_I("J8108", "feedback OK (+%lu frames) -> CAN link verified",
                          (unsigned long)(s_dev->fb.rx_count - s_mark_count));
                    LOG_I("J8108", "-> DAMP hold (you may turn the output shaft by hand and watch numbers)");
                    phase_enter(PH_DAMP);
                }
                else if (s_try_cnt < J8108_ENABLE_RETRY_MAX)
                {
                    s_try_cnt++;
                    J8108_SendCmd(J8108_CMD_ENABLE);
                    LOG_W("J8108", "no feedback yet -> resend ENABLE (attempt %lu)", (unsigned long)s_try_cnt);
                    s_phase_start_ms = now_ms();
                }
                else
                {
                    LOG_E("J8108", "no feedback after %lu enable attempts. Check: 1) CAN_H/L wiring 2) motor 24V 3) 120R switch 4) motor CANID=0x01",
                          (unsigned long)s_try_cnt);
                    LOG_E("J8108", "-> DONE (no motion command sent, safe exit)");
                    phase_enter(PH_DONE);
                }
            }
            break;

        case PH_DAMP:
            if (elapsed >= J8108_DAMP_MS)
            {
                LOG_I("J8108", "-> SPIN_FWD %.2f rad/s (output shaft)", J8108_SPIN_SPEED_RADS);
                phase_enter(PH_SPIN_FWD);
            }
            break;

        case PH_SPIN_FWD:
            if (elapsed >= J8108_SPIN_MS)
            {
                LOG_I("J8108", "-> SPIN_REV");
                phase_enter(PH_SPIN_REV);
            }
            break;

        case PH_SPIN_REV:
            if (elapsed >= J8108_SPIN_MS)
            {
                LOG_I("J8108", "-> DAMP2");
                phase_enter(PH_DAMP2);
            }
            break;

        case PH_DAMP2:
            if (elapsed >= J8108_DAMP2_MS)
            {
                J8108_SendCmd(J8108_CMD_DISABLE);
                LOG_I("J8108", "-> sent DISABLE frame FF FF FF FF FF FF FF FD. Test done (reset board to rerun)");
                phase_enter(PH_DONE);
            }
            break;

        case PH_DONE:
        default:
            break;
        }

        /* ---- 反馈打印（运动阶段 500ms 一次） ---- */
        if (t - last_print_ms >= J8108_PRINT_PERIOD_MS)
        {
            last_print_ms = t;
            if (s_phase >= PH_DAMP && s_phase <= PH_DAMP2)
            {
                print_feedback();
            }
        }

        vTaskDelay(1);
    }
}

void J8108_Task_Init(void)
{
    BaseType_t ok = xTaskCreate(j8108_task, "J8108", J8108_TASK_STACK_WORDS, NULL, J8108_TASK_PRIORITY, &s_task_handle);

    if (ok != pdPASS)
    {
        LOG_E("J8108", "xTaskCreate FAILED (heap/stack?) -> MOTOR TASK DISABLED");
    }
    else
    {
        LOG_I("J8108", "task created (J8108, prio %u, stack %u words)", J8108_TASK_PRIORITY, J8108_TASK_STACK_WORDS);
    }
}
