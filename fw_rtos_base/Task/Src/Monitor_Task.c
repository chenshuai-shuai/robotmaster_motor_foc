/*
 * Monitor_Task.c - 低频监控任务实现（屏刷 10Hz / 日志 1Hz / 遥测 20Hz）
 *
 * 设计要点：
 *   1. 三个出口**同源**：都从 J8108_CopySnapshot() 的帧一致快照取值 → 屏/日志/遥测永不打架
 *   2. **只在数据新鲜时**输出数值（age < 200ms）；链路断只报一次 LOST 边沿 + 10s 摘要（绝不重播旧数据）
 *   3. 按键 → 纯 UI（翻页），本任务消费全部按键事件位（单击/双击/长按/卡死），不做任何电机动作
 *   4. 1Hz 日志与 10s 摘要集中在本任务（控制任务只管事件边沿），日志出口唯一、好读
 */
#include "Monitor_Task.h"

#include "FreeRTOS.h"
#include "task.h"
#include "event_groups.h"

#include <stdio.h>
#include <string.h>

#include "bsp_log.h"
#include "stack_probe.h" /* 栈余量自报 */
#include "disp_port.h"   /* Disp_LastFlushUs：10s 摘要里的屏刷耗时（TM-D2 判据） */
#include "motor_8108.h"
#include "ctrl_core.h"
#include "ui_action.h"
#include "bsp_key.h"
#include "Oled_Task.h"
#include "J8108_Task.h"
#include "CmdRx_Task.h"
#include "proto_tx.h"
#include "version.h"
#include "sys_status.h"
#include "feature_config.h"

#if FEATURE_MONITOR_TASK
/* M3 文件级隔离（docs/规范_功能宏与模块化.md R3）：未启用时本文件编译为空对象。
 * 被谁调用必须由调用点用同一个宏保护（忘保护=链接失败，这是刻意设计的 fail-fast）。 */

#define MON_TASK_PRIORITY (3U)
#define MON_TASK_STACK_WORDS (640U) /* snprintf 浮点 + 渲染缓冲 */
#define MON_TICK_MS (10U)           /* 任务节拍 */
#define MON_UI_MS (100U)            /* 屏刷周期 10Hz */
#define MON_LOG_MS (1000U)          /* 数据日志周期 1Hz */
#define MON_SUM_MS (10000U)         /* 状态摘要周期 10s */
#define MON_FRESH_MS (200U)         /* 数据新鲜门槛（与 J8108 一致）*/
#define MON_DEF_TEL_MS (0u)         /* 遥测默认关（上位机显式开），最大 1000ms */

static TaskHandle_t s_task;
static volatile uint16_t s_tel_period_ms = MON_DEF_TEL_MS;
static Ui_Status_t s_ui;
static volatile uint32_t s_last_loop_ms; /* 循环心跳（#ST monitor 自检用） */

void Monitor_SetTelPeriod(uint16_t ms)
{
    s_tel_period_ms = (ms < 2u) ? 0u : ms;
}

uint16_t Monitor_GetTelPeriod(void)
{
    return s_tel_period_ms;
}

const Ui_Status_t *Monitor_Ui(void)
{
    return &s_ui;
}

static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

/* 遥测行（与协议 §5.5 一致）：@TEL ms=.. mode=.. en=.. pos=.. vel=.. T=.. Tm=.. Tr=.. err=0x.. set=.. age=.. */
void Monitor_FormatTel(char *buf, uint32_t bufsz)
{
    J8108_Snapshot_t sn;
    const Ctrl_State_t *st = J8108_State();

    if ((buf == NULL) || (bufsz == 0u))
        return;

    J8108_CopySnapshot(&sn);
    (void)snprintf(buf, bufsz,
                   "@TEL ms=%lu mode=%s en=%u pos=%.2f vel=%.1f T=%.3f Tm=%.1f Tr=%.1f err=0x%02X set=%.2f age=%lu",
                   (unsigned long)now_ms(), Ctrl_ModeStr(st->mode), (unsigned)st->enabled,
                   (double)sn.pos_deg, (double)sn.vel_dps, (double)sn.torque,
                   (double)sn.t_mos, (double)sn.t_rotor, sn.err, (double)st->set,
                   (unsigned long)sn.frame_age_ms);
}

/* ------------------------------ 组装 UI 状态 ------------------------------ */
static void ui_fill(Ui_Status_t *ui, const J8108_Snapshot_t *sn, uint16_t fb_hz)
{
    const Ctrl_State_t *st = J8108_State();
    Ctrl_Params_t *pp = J8108_Params();
    uint32_t now = now_ms();
    uint32_t wd_left = 0u;
    float kp = 0.0f;
    float kd = 0.0f;

    ui->mode = (uint8_t)st->mode;
    ui->enabled = st->enabled;
    ui->frames_on = J8108_IsSending();
    ui->inpos = st->inpos;
    ui->ev_flags = J8108_EventLatch();
    ui->fb_hz = fb_hz;

    ui->set_deg = 0.0f;
    ui->set_dps = 0.0f;
    ui->set_nm = 0.0f;
    switch (st->mode)
    {
    case CTRL_MODE_POS:
        ui->set_deg = st->set_applied;
        break;
    case CTRL_MODE_SPEED:
        ui->set_dps = st->set_applied;
        break;
    case CTRL_MODE_TORQUE:
    case CTRL_MODE_IMP:
        ui->set_nm = st->set;
        break;
    default:
        break;
    }

    J8108_DbgFrames(&kp, &kd);
    ui->kp_now = kp;
    ui->kd_now = kd;

    if ((pp->wd_ms != 0u) && (st->enabled != 0u))
    {
        uint32_t used = (uint32_t)(now - st->last_cmd_ms);
        wd_left = (used >= (uint32_t)pp->wd_ms) ? 0u : (uint32_t)((uint32_t)pp->wd_ms - used);
    }
    ui->wd_left_ms = (uint16_t)wd_left;

    ui->rx_lines = 0u;
    ui->tx_lines = Proto_TxLines();
    ui->rx_bytes = 0u;
    ui->err_code = 0u;
    ui->last_cmd[0] = '-';
    ui->last_cmd[1] = '\0';
#if FEATURE_SERIAL_CTRL
    /* 协议层未编译时（M3 文件级隔离）这些计数器不存在 → 页面显示 "-"/0（诚实，不编造） */
    ui->rx_lines = CmdRx_RxLines();
    ui->rx_bytes = CmdRx_RxBytes();
    ui->err_code = CmdRx_LastErr();
    (void)snprintf(ui->last_cmd, sizeof(ui->last_cmd), "%s", CmdRx_LastCmd());
#endif
    ui->tel_period_ms = s_tel_period_ms;

    ui->uptime_s = (uint32_t)(xTaskGetTickCount() / configTICK_RATE_HZ);
    ui->free_heap = (uint32_t)xPortGetFreeHeapSize();
    ui->task_cnt = (uint32_t)uxTaskGetNumberOfTasks();
}

/* 按键 → 纯 UI 动作（**不做任何电机动作**） */
static void ui_key_poll(Ui_Status_t *ui)
{
    EventBits_t bits;
    uint32_t hold_ms = 0u;
    Ui_Action_t act;

    if (Key_EventGroup() == NULL)
    {
        return;
    }
    bits = xEventGroupWaitBits(Key_EventGroup(),
                               KEY_BIT_CLICK | KEY_BIT_DOUBLE | KEY_BIT_HOLD_RELEASE | KEY_BIT_STUCK,
                               pdTRUE, pdFALSE, 0);
    if (bits == 0u)
    {
        return;
    }

    if ((bits & KEY_BIT_STUCK) != 0u)
    {
        act = Ui_ActionDecide(UI_GES_STUCK, 0u, UI_HOME_HOLD_MS); /* 卡死作废 */
    }
    else if ((bits & KEY_BIT_HOLD_RELEASE) != 0u)
    {
        KeyCore_Msg_t msg;

        (void)memset(&msg, 0, sizeof(msg));
        Key_GetLastMsg(&msg, NULL);
        hold_ms = msg.hold_ms;
        act = Ui_ActionDecide(UI_GES_HOLD, hold_ms, UI_HOME_HOLD_MS);
    }
    else if ((bits & KEY_BIT_DOUBLE) != 0u)
    {
        act = Ui_ActionDecide(UI_GES_DOUBLE, 0u, UI_HOME_HOLD_MS);
    }
    else
    {
        act = Ui_ActionDecide(UI_GES_CLICK, 0u, UI_HOME_HOLD_MS);
    }

    switch (act)
    {
    case UI_ACT_PAGE_NEXT:
        ui->page = Ui_PageNext(ui->page, ui->page_cnt);
        LOG_I("Monitor", "key: page -> %u/%u", (unsigned)(ui->page + 1u), (unsigned)ui->page_cnt);
        break;
    case UI_ACT_PAGE_PREV:
        ui->page = Ui_PagePrev(ui->page, ui->page_cnt);
        LOG_I("Monitor", "key: page -> %u/%u", (unsigned)(ui->page + 1u), (unsigned)ui->page_cnt);
        break;
    case UI_ACT_PAGE_HOME:
        ui->page = 0u;
        LOG_I("Monitor", "key: page -> home (1/%u)", (unsigned)ui->page_cnt);
        break;
    default:
        break; /* NONE：静默（含卡死作废） */
    }
}

/* ------------------------------ 任务主体 ------------------------------ */
static void monitor_task(void *arg)
{
    uint32_t t_last_ui = 0u;
    uint32_t t_last_log = 0u;
    uint32_t t_last_sum = 0u;
    uint32_t t_last_tel = 0u;
    uint32_t hz_win_ms = now_ms();
    uint32_t hz_win_cnt = 0u;
    uint16_t fb_hz = 0u;

    (void)arg;

    (void)memset(&s_ui, 0, sizeof(s_ui));
    s_ui.page = UI_PAGE_LINK;
    s_ui.page_cnt = UI_PAGE_COUNT;

    LOG_I("Monitor", "task up: screen 10Hz / log 1Hz / telemetry(period=%ums) - all from the SAME snapshot",
          (unsigned)s_tel_period_ms);
    LOG_I("Monitor", "pages: 0=LINK 1=MOTION 2=SERIAL 3=SYSTEM | key: click=next dbl=prev hold=home (UI ONLY, no motor action)");

    for (;;)
    {
        uint32_t t = now_ms();
        J8108_Snapshot_t sn;

        s_last_loop_ms = t; /* #ST monitor 自检心跳：循环活着就打点（不依赖后面任何分支） */

        stack_probe_tick(t, "Monitor", MON_TASK_STACK_WORDS); /* 栈余量自报（5s 一次） */
        uint8_t fresh;

        J8108_CopySnapshot(&sn);
        fresh = ((sn.rx_count > 0u) && (sn.frame_age_ms < (uint32_t)MON_FRESH_MS)) ? 1u : 0u;

        /* 反馈帧率（1s 窗口） */
        if ((uint32_t)(t - hz_win_ms) >= 1000u)
        {
            fb_hz = (uint16_t)(sn.rx_count - hz_win_cnt);
            hz_win_cnt = sn.rx_count;
            hz_win_ms = t;
        }

        ui_key_poll(&s_ui);
        ui_fill(&s_ui, &sn, fb_hz);

        /* ---- 1. 屏幕刷新（10Hz，局部刷新） ---- */
        if ((uint32_t)(t - t_last_ui) >= (uint32_t)MON_UI_MS)
        {
            t_last_ui = t;
            Oled_UiDraw(s_ui.page, &sn, &s_ui);
        }

        /* ---- 2. 数据日志（1Hz；只在数据新鲜时打数值） ---- */
        if ((uint32_t)(t - t_last_log) >= (uint32_t)MON_LOG_MS)
        {
            t_last_log = t;
            if (fresh != 0u)
            {
                LOG_I("Monitor", "data: pos=%.2fdeg vel=%.1fdps T=%.3fNm Tm=%.1f Tr=%.1f err=0x%02X mode=%s en=%u set=%.2f age=%lums",
                      (double)sn.pos_deg, (double)sn.vel_dps, (double)sn.torque, (double)sn.t_mos, (double)sn.t_rotor,
                      sn.err, Ctrl_ModeStr((Ctrl_Mode_e)s_ui.mode), (unsigned)s_ui.enabled,
                      (double)((s_ui.mode == (uint8_t)CTRL_MODE_POS) ? s_ui.set_deg : ((s_ui.mode == (uint8_t)CTRL_MODE_SPEED) ? s_ui.set_dps : s_ui.set_nm)),
                      (unsigned long)sn.frame_age_ms);
            }
            /* 不新鲜 → 静默（LOST 边沿日志已在控制任务打过，绝不重播旧数值） */
        }

        /* ---- 3. 状态摘要（10s） ---- */
        if ((uint32_t)(t - t_last_sum) >= (uint32_t)MON_SUM_MS)
        {
            t_last_sum = t;
            /* 屏刷耗时 + 推屏次数单独一行（TM-D2 判据）：不并进 summary 行 —— 那行已经接近
             * LOG_FMT_BUF_SIZE(128) 的截断线，塞在末尾正好会被截掉（M1 实测的坑）。
             * **pushes= 是必需的**：画面静止时 flush= 数值会一直不变（不推屏），
             * 只有对比两次的 pushes 增量才能区分"没变化（正常）"与"推屏停了（故障）"。 */
            LOG_I("disp", "flush=%luus pushes=%lu", (unsigned long)Disp_LastFlushUs(),
                  (unsigned long)Disp_PushCount());
            if (fresh != 0u)
            {
                LOG_I("Monitor", "summary: link=UP mode=%s en=%u rx=%lu tx=%lu hz=%u uptime=%lus heap=%lu txdrop=%lu dtmax=%lums",
                      Ctrl_ModeStr((Ctrl_Mode_e)s_ui.mode), (unsigned)s_ui.enabled,
                      (unsigned long)sn.rx_count, (unsigned long)sn.tx_cnt, (unsigned)fb_hz,
                      (unsigned long)s_ui.uptime_s, (unsigned long)s_ui.free_heap,
                      (unsigned long)Proto_TxBusyDrops(), (unsigned long)J8108_LoopJitterTake());
            }
            else
            {
                LOG_I("Monitor", "summary: link=DOWN (no fresh frame for %lums) mode=%s en=%u rx=%lu tx=%lu | stale values not printed on purpose",
                      (unsigned long)sn.frame_age_ms, Ctrl_ModeStr((Ctrl_Mode_e)s_ui.mode),
                      (unsigned)s_ui.enabled, (unsigned long)sn.rx_count, (unsigned long)sn.tx_cnt);
            }
        }

        /* ---- 4. 协议遥测（@TEL，默认关；#TEL 开） ---- */
        if (s_tel_period_ms >= 2u)
        {
            if ((uint32_t)(t - t_last_tel) >= (uint32_t)s_tel_period_ms)
            {
                char b[160];
                t_last_tel = t;
                if (fresh != 0u)
                {
                    Monitor_FormatTel(b, sizeof(b));
                    Proto_Send(b);
                }
                else
                {
                    /* 不新鲜：只报链路状态，不报旧数值 */
                    (void)snprintf(b, sizeof(b), "@TEL ms=%lu mode=%s en=%u link=DOWN age=%lu",
                                   (unsigned long)t, Ctrl_ModeStr((Ctrl_Mode_e)s_ui.mode), (unsigned)s_ui.enabled,
                                   (unsigned long)sn.frame_age_ms);
                    Proto_Send(b);
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(MON_TICK_MS));
    }
}

void Monitor_Task_Init(void)
{
    BaseType_t ok = xTaskCreate(monitor_task, "Monitor", MON_TASK_STACK_WORDS, NULL, MON_TASK_PRIORITY, &s_task);

    if (ok != pdPASS)
    {
        LOG_E("Monitor", "xTaskCreate FAILED (heap/stack?) -> SCREEN/LOG/TELEMETRY DISABLED");
    }
}

uint8_t Monitor_SelfTest(void)
{
    uint32_t age;

    if (s_task == NULL)
        return 3u; /* 监视任务没起来（堆不足？） */
    age = now_ms() - s_last_loop_ms;
    return (age < 500u) ? 1u : 2u; /* 循环 100ms 一次；500ms 没动静 = 卡住/被饿死 */
}

#endif /* FEATURE_MONITOR_TASK */
