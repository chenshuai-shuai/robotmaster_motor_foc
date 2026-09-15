/*
 * Oled_Task.c - 单页监护界面（M1 骨架版：只读显示 + 上下选中光标 + 长按进度条）
 *
 * 界面：6×8 字体 → 8 行 × 21 字符（OLED 128×64，软 I2C PB10=SCL/PB9=SDA）
 *   行0  CAN 状态 + 反馈帧率 + 使能状态   ← 唯一绑定动作的行（M3 接使能/失能）
 *   行1  位置（电机端 rad / 输出端 度）
 *   行2  速度（rad/s 输出端 / RPM）
 *   行3  前馈扭矩 N·m
 *   行4  MOS 温度 / 线圈温度
 *   行5  反馈帧计数 RX / 发送帧计数 TX
 *   行6  诊断：ΔT（距上一帧）+ CAN 错误状态；按住按键时改为长按进度条
 *   行7  按键提示（2s）> 系统异常 > SD 异常 > 版本 + 运行秒
 *
 * 交互（M1）：
 *   单击 = 光标下移（回绕）· 双击 = 光标上移 · 长按 = 进度条（M1 不接动作，M3 接）
 *   注意：6×8 字体是 ASCII 字体 → 屏上只用 ASCII（中文需 16×16 汉字单元，本页不用）
 *
 * 并发：显示任务优先级 3（低于控制 5 / 按键 4）—— 软 I2C 刷屏不能挤控制任务；
 *       数据只从 J8108 模块"只读"，本任务不发任何 CAN 帧。
 */
#include "Oled_Task.h"

#include "FreeRTOS.h"
#include "task.h"
#include "event_groups.h"
#include "stdio.h"

#include "OLED.h"
#include "bsp_log.h"
#include "bsp_key.h"
#include "motor_8108.h"
#include "version.h"
#include "sys_status.h"
#include "feature_config.h"
#include "j8108_action.h" /* ACTR_* 动作结果（行7 显示） */
#include "J8108_Task.h"   /* J8108_LastAction() */

/* ------------------------------ 任务参数 ------------------------------ */
#define OLED_TASK_PRIORITY (3U)
#define OLED_TASK_STACK_WORDS (640U) /* 含 snprintf(浮点) → 留裕量 */
#define OLED_REFRESH_PERIOD_MS (200U)

/* ------------------------------ 界面参数 ------------------------------ */
#define UI_ROW_H (8U)
#define UI_ROWS (8U)
#define UI_ACTION_ROW (0U)      /* 唯一绑定动作的行（M3：使能/失能） */
#define UI_HOLD_EXEC_MS (2000U) /* 该行的最短按住时长（M3 用；计时不上屏） */
#define UI_HINT_SHOW_MS (2000U)

extern volatile int g_rw_result; /* SD 读写自检结果（sd_diskio.c） */

static TaskHandle_t s_oled_task;
static volatile uint8_t s_cursor_row = UI_ACTION_ROW; /* 当前光标行（动作层经 Oled_UiGetCursorRow 读取） */

/* 行名（调试日志用，ASCII） */
static const char *const s_row_name[UI_ROWS] = {"CAN", "POS", "VEL", "T", "TEMP", "COUNT", "DIAG", "INFO"};

static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

/* 画一行：先写字，选中行再整行反色 */
static void ui_row(uint8_t row, const char *txt, uint8_t selected)
{
    OLED_ShowString(0U, (uint8_t)(row * UI_ROW_H), txt, OLED_6X8);
    if (selected != 0U)
    {
        OLED_ReverseArea(0U, (uint8_t)(row * UI_ROW_H), 128U, UI_ROW_H);
    }
}

static void oled_task(void *arg)
{
    uint8_t cursor = UI_ACTION_ROW;
    uint32_t hint_until_ms = 0U;
    uint32_t hz_win_ms = now_ms();
    uint32_t hz_win_cnt = 0U;
    uint16_t fb_hz = 0U;
    char line[24];

    (void)arg;

    LOG_I("OledTask", "start: single-page monitor UI (8 rows x 21 cols, 6x8 font, ASCII only)");
    LOG_I("OledTask", "row0 = CAN link state (INIT FAIL / INIT OK WAITING / READY / BUS ERR); no timer on screen by design");

    for (;;)
    {
        uint32_t t = now_ms();
        J8108_Snapshot_t sn; /* M2：整屏取自"帧一致"快照，不直读驱动内部结构（避免与 200Hz 控制/中断撕数据） */
        uint32_t rx;
        uint32_t dt;
        uint8_t have;

        J8108_CopySnapshot(&sn);
        rx = sn.rx_count;
        dt = sn.frame_age_ms;
        have = sn.valid;

        /* ---- 1. 按键事件：光标移动（纯 UI，零副作用） ----
           本任务只消费 CLICK/DOUBLE 两个位；HOLD_RELEASE/STUCK 位留给动作层(M3)，
           提示行改用"最近事件时间"判定 → 两个任务不会互抢同一个事件位 */
        if (Key_EventGroup() != NULL)
        {
            EventBits_t bits = xEventGroupWaitBits(Key_EventGroup(), KEY_BIT_CLICK | KEY_BIT_DOUBLE,
                                                   pdTRUE, pdFALSE, 0);
            uint32_t last_msg_ms = 0u;

            if ((bits & KEY_BIT_CLICK) != 0U)
            {
                cursor = (uint8_t)((cursor + 1U) % UI_ROWS);
                LOG_I("OledTask", "CLICK -> cursor row %u (%s)", cursor, s_row_name[cursor % UI_ROWS]);
            }
            if ((bits & KEY_BIT_DOUBLE) != 0U)
            {
                cursor = (uint8_t)((cursor + UI_ROWS - 1U) % UI_ROWS);
                LOG_I("OledTask", "DOUBLE -> cursor row %u (%s)", cursor, s_row_name[cursor % UI_ROWS]);
            }
            Key_GetLastMsg(NULL, &last_msg_ms);
            if (last_msg_ms != 0u)
            {
                hint_until_ms = last_msg_ms + UI_HINT_SHOW_MS;
            }
        }
        s_cursor_row = cursor; /* 发布给动作层：长按作用于"光标选中行" */

        /* ---- 2. 反馈帧率（1s 窗口） ---- */
        if ((t - hz_win_ms) >= 1000U)
        {
            fb_hz = (uint16_t)(rx - hz_win_cnt);
            hz_win_cnt = rx;
            hz_win_ms = t;
        }

        OLED_Clear();

        /* 行0：CAN 链路可视化（用户要求：初始化 OK / 通道就绪可发数据 / 失败上屏；失败不重试）
           四态来自 motor_8108 的 J8108_Status_e（快照内） */
        if (sn.status == (uint8_t)J8108_ST_INIT_FAIL)
        {
            snprintf(line, sizeof(line), "CAN INIT FAIL");
        }
        else if (sn.status == (uint8_t)J8108_ST_BUS_ERR)
        {
            snprintf(line, sizeof(line), "CAN BUS ERR 0x%04lX", (unsigned long)(sn.bus_err & 0xFFFFu));
        }
        else if (sn.status == (uint8_t)J8108_ST_READY)
        {
            snprintf(line, sizeof(line), "CAN READY %4uHz %s", (unsigned)fb_hz, (J8108_IsHoldMode() != 0U) ? "HOLD" : "SEND");
        }
        else if (sn.status == (uint8_t)J8108_ST_INIT_OK)
        {
            snprintf(line, sizeof(line), "CAN INIT OK WAITING");
        }
        else
        {
            snprintf(line, sizeof(line), "CAN STATUS ?");
        }
        ui_row(0U, line, (cursor == 0U));

        /* 行1：位置（电机端 rad / 输出端 度） */
        if (have == 0U)
        {
            snprintf(line, sizeof(line), "POS   ---.--r  -----");
        }
        else
        {
            snprintf(line, sizeof(line), "POS %7.2fr %5.1fd", (double)sn.pos, (double)sn.pos_out_deg);
        }
        ui_row(1U, line, (cursor == 1U));

        /* 行2：速度（rad/s 输出端 / RPM） */
        if (have == 0U)
        {
            snprintf(line, sizeof(line), "VEL   --.--r/s  ----");
        }
        else
        {
            snprintf(line, sizeof(line), "VEL %6.2fr/s %5.1fR", (double)sn.vel, (double)sn.vel_rpm);
        }
        ui_row(2U, line, (cursor == 2U));

        /* 行3：前馈扭矩 */
        if (have == 0U)
        {
            snprintf(line, sizeof(line), "T     --.---Nm");
        }
        else
        {
            snprintf(line, sizeof(line), "T %8.3fNm", (double)sn.torque);
        }
        ui_row(3U, line, (cursor == 3U));

        /* 行4：两路温度（超温阈值 100℃ → 反色告警） */
        if (have == 0U)
        {
            snprintf(line, sizeof(line), "MOS  --C COIL  --C");
        }
        else
        {
            snprintf(line, sizeof(line), "MOS %3.0fC COIL%3.0fC", (double)sn.t_mos, (double)sn.t_rotor);
        }
        ui_row(4U, line, (cursor == 4U) || (have != 0U && (sn.t_mos > 100.0f || sn.t_rotor > 100.0f)));

        /* 行5：帧计数 */
        snprintf(line, sizeof(line), "RX%7lu TX%6lu", (unsigned long)rx, (unsigned long)sn.tx_cnt);
        ui_row(5U, line, (cursor == 5U));

        /* ---- 行6：CAN 诊断。总线正常：距上一帧时间 + HAL 错误码；总线异常：TEC/REC/LEC（判"没人应答"）---- */
        if (sn.bus_err != 0u)
        {
            uint32_t esr = hcan1.Instance->ESR;

            snprintf(line, sizeof(line), "TEC%3lu REC%3lu LEC%lu",
                     (unsigned long)((esr & CAN_ESR_TEC) >> CAN_ESR_TEC_Pos),
                     (unsigned long)((esr & CAN_ESR_REC) >> CAN_ESR_REC_Pos),
                     (unsigned long)((esr & CAN_ESR_LEC) >> CAN_ESR_LEC_Pos));
        }
        else
        {
            snprintf(line, sizeof(line), "dT %4ums ES %04lX", (unsigned)dt, (unsigned long)(sn.bus_err & 0xFFFFu));
        }
        ui_row(6U, line, (cursor == 6U));

        /* 行7：动作结果(2.5s) > 按键提示(2s) > 系统异常 > SD 异常 > 版本+运行时间 */
        {
            uint32_t act_age = 0u;
            J8108_ActionResult_e ar = J8108_LastAction(&act_age);

            if ((ar != ACTR_NONE) && (act_age < 2500u))
            {
                snprintf(line, sizeof(line), "%s", J8108_ActionResultStr(ar));
            }
            else if (t < hint_until_ms)
            {
                snprintf(line, sizeof(line), "CLK=v DBL=^ HOLD=EXEC");
            }
            else if (SYS_GetState() != SYS_STATE_RUNNING)
            {
                snprintf(line, sizeof(line), "SYS:%s", SYS_StateText(SYS_GetState()));
            }
            else if (g_rw_result > 0)
            {
                snprintf(line, sizeof(line), "SD: RW FAIL");
            }
            else
            {
                snprintf(line, sizeof(line), "%s UP%lus", FW_VERSION_STR, (unsigned long)(xTaskGetTickCount() / configTICK_RATE_HZ));
            }
        }
        ui_row(7U, line, (cursor == 7U));

        OLED_Update();

        vTaskDelay(pdMS_TO_TICKS(OLED_REFRESH_PERIOD_MS));
    }
}

void Oled_Task_Init(void)
{
    BaseType_t ok = xTaskCreate(oled_task, "OledTask", OLED_TASK_STACK_WORDS, NULL, OLED_TASK_PRIORITY, &s_oled_task);

    if (ok != pdPASS)
    {
        LOG_E("OledTask", "xTaskCreate FAILED (heap/stack?) -> UI DISABLED");
    }
}

uint8_t Oled_UiGetCursorRow(void)
{
    return s_cursor_row;
}
