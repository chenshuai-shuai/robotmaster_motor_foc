/*
 * Oled_Task.c - OLED 4 页监护界面渲染器（软 I2C PB10=SCL / PB9=SDA，SH1106，6x8 字体 → 8 行 x 21 字符）
 *
 * 页面（docs/协议_串口控制_v1.md §13）：
 *   P0 LINK   : CAN 四态 + rx/tx + 回传 Hz + ERR 报错位 + 总线诊断
 *   P1 MOTION : 模式/使能 + pos/vel/T + 设定点 + 本帧 Kp/Kd + 双温度 + 到位
 *   P2 SERIAL : 协议态（最近命令 / 行计数 / 心跳剩余 / 电机报错 / 协议错误 / 遥测周期）
 *   P3 SYSTEM : 版本 / 运行时间 / free heap / 任务数 / 保护事件 / 位置量程
 *
 * 设计：
 *   · **局部刷新**：每行与上次比较，只有变化才重画并触发 OLED_Update()（整屏软 I2C ≈10ms，
 *     局部 1~2 行 ≈1.3~2.6ms；10Hz 下约 2.6% CPU）
 *   · 本文件**不发任何 CAN 帧**、不碰控制状态（只读快照/只读状态视图）
 *   · 只用 ASCII（6x8 字体无中文）
 */
#include "Oled_Task.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#include "OLED.h"
#include "main.h"      /* hcan1 / CAN_ESR_* 宏 */
#include "bsp_log.h"
#include "ctrl_core.h"
#include "version.h"
#include "feature_config.h"   /* M3：文件级隔离需要一个统一的开关 */


#if FEATURE_DISP_UI
/* M3 文件级隔离（docs/规范_功能宏与模块化.md R3）：未启用时本文件编译为空对象。
 * 被谁调用必须由调用点用同一个宏保护（忘保护=链接失败，这是刻意设计的 fail-fast）。 */

#define UI_COLS (21u)
#define UI_ROWS (8u)

static char s_last[UI_ROWS][UI_COLS + 1u];
static uint8_t s_page = 0xFFu; /* 0xFF = 未绘制过（强制全刷） */
static volatile uint32_t s_draw_ms; /* 最近一次绘制时刻（#ST disp 自检用） */

/* 画一行：格式化 → 补空格到 21 列 → 与上次比较 → 变化才写屏 */
static void rfmt(uint8_t row, int force, const char *fmt, ...)
{
    char tmp[64];
    char out[UI_COLS + 1u];
    va_list ap;
    uint8_t n;
    uint8_t i;

    if (row >= UI_ROWS)
        return;

    va_start(ap, fmt);
    (void)vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);

    n = (uint8_t)strlen(tmp);
    if (n > (uint8_t)UI_COLS)
    {
        n = (uint8_t)UI_COLS;
    }
    (void)memcpy(out, tmp, n);
    for (i = n; i < (uint8_t)UI_COLS; i++)
    {
        out[i] = ' ';
    }
    out[UI_COLS] = '\0';

    if ((force == 0) && (memcmp(out, s_last[row], UI_COLS) == 0))
    {
        return; /* 无变化：不刷 */
    }
    (void)memcpy(s_last[row], out, UI_COLS);
    OLED_ShowString(0u, (uint8_t)(row * 8u), out, OLED_6X8);
}

static const char *bus_state_str(uint8_t status)
{
    switch (status)
    {
    case J8108_ST_INIT_FAIL:
        return "INIT FAIL";
    case J8108_ST_INIT_OK:
        return "INIT OK WAIT";
    case J8108_ST_READY:
        return "READY";
    case J8108_ST_BUS_ERR:
        return "BUS ERR";
    default:
        return "?";
    }
}

void Oled_UiDraw(uint8_t page, const J8108_Snapshot_t *sn, const Ui_Status_t *ui)
{
    int force;
    uint8_t have;
    uint8_t fresh;

    if ((sn == NULL) || (ui == NULL))
        return;
    if (page >= UI_PAGE_COUNT)
        page = 0u;

    s_draw_ms = HAL_GetTick(); /* 自检心跳（只写一个 volatile，无阻塞、不影响刷新逻辑） */

    force = (page != s_page) ? 1 : 0;
    if (force != 0)
    {
        s_page = page;
        OLED_Clear();
        (void)memset(s_last, 0, sizeof(s_last)); /* 清空缓存 → 全刷 */
        force = 1;
    }

    have = sn->valid;
    fresh = ((have != 0u) && (sn->frame_age_ms < 200u)) ? 1u : 0u;

    switch (page)
    {
    /* ---------------------------------- P0 LINK ---------------------------------- */
    case UI_PAGE_LINK:
        rfmt(0u, force, "LINK %u/%u %s", (unsigned)(page + 1u), (unsigned)ui->page_cnt, bus_state_str(sn->status));

        rfmt(1u, force, "RX %lu TX %lu", (unsigned long)sn->rx_count, (unsigned long)sn->tx_cnt);

        if (sn->err == 0u)
        {
            rfmt(2u, force, "ERR OK (%s)", J8108_ErrStr(sn->err));
        }
        else
        {
            rfmt(2u, force, "ERR 0x%02X %s", sn->err, J8108_ErrStr(sn->err));
        }

        if (sn->bus_err != 0u)
        {
            uint32_t esr = hcan1.Instance->ESR;

            rfmt(3u, force, "TEC%3lu REC%3lu LEC%lu",
                 (unsigned long)((esr & CAN_ESR_TEC) >> CAN_ESR_TEC_Pos),
                 (unsigned long)((esr & CAN_ESR_REC) >> CAN_ESR_REC_Pos),
                 (unsigned long)((esr & CAN_ESR_LEC) >> CAN_ESR_LEC_Pos));
        }
        else
        {
            rfmt(3u, force, "BUS OK (HAL 0x%04lX)", (unsigned long)(sn->bus_err & 0xFFFFu));
        }

        if (sn->rx_count == 0u)
        {
            rfmt(4u, force, "AGE never  LINK DOWN"); /* 从未收到过反馈帧（不是"年龄 0ms"） */
        }
        else if (fresh != 0u)
        {
            rfmt(4u, force, "AGE %lums  %uHz", (unsigned long)sn->frame_age_ms, (unsigned)ui->fb_hz);
        }
        else
        {
            rfmt(4u, force, "AGE %lums  LINK DOWN", (unsigned long)sn->frame_age_ms);
        }

        rfmt(5u, force, "MODE %s EN %u", Ctrl_ModeStr((Ctrl_Mode_e)ui->mode), (unsigned)ui->enabled);
        rfmt(6u, force, "SET %8.2f %s", (double)ui->set_deg, "deg");
        rfmt(7u, force, "KEY CLK=next DBL=prev");
        break;

    /* --------------------------------- P1 MOTION --------------------------------- */
    case UI_PAGE_MOTION:
        rfmt(0u, force, "MOTION %u/%u %s", (unsigned)(page + 1u), (unsigned)ui->page_cnt,
             Ctrl_ModeStr((Ctrl_Mode_e)ui->mode));

        if (fresh != 0u)
        {
            rfmt(1u, force, "POS %9.2f deg", (double)sn->pos_deg);
            rfmt(2u, force, "VEL %9.2f dps", (double)sn->vel_dps);
            rfmt(3u, force, "T   %9.3f Nm", (double)sn->torque);
            rfmt(6u, force, "Tm %4.1fC Tr %4.1fC", (double)sn->t_mos, (double)sn->t_rotor);
        }
        else
        {
            rfmt(1u, force, "POS      ---- deg");
            rfmt(2u, force, "VEL      ---- dps");
            rfmt(3u, force, "T        ---- Nm");
            rfmt(6u, force, "Tm   --.-C Tr  --.-C");
        }

        switch ((Ctrl_Mode_e)ui->mode)
        {
        case CTRL_MODE_POS:
            rfmt(4u, force, "SET %9.2f deg", (double)ui->set_deg);
            break;
        case CTRL_MODE_SPEED:
            rfmt(4u, force, "SET %9.2f dps", (double)ui->set_dps);
            break;
        case CTRL_MODE_TORQUE:
        case CTRL_MODE_IMP:
            rfmt(4u, force, "SET %9.3f Nm", (double)ui->set_nm);
            break;
        default:
            rfmt(4u, force, "SET      ---- ");
            break;
        }

        rfmt(5u, force, "KP %5.1f KD %5.2f", (double)ui->kp_now, (double)ui->kd_now);
        rfmt(7u, force, "EN %u SEND %u INPOS %u", (unsigned)ui->enabled, (unsigned)ui->frames_on, (unsigned)ui->inpos);
        break;

    /* --------------------------------- P2 SERIAL --------------------------------- */
    case UI_PAGE_SERIAL:
        rfmt(0u, force, "SERIAL %u/%u", (unsigned)(page + 1u), (unsigned)ui->page_cnt);
        rfmt(1u, force, "LAST #%s", (ui->last_cmd[0] != '\0') ? ui->last_cmd : "-");
        rfmt(2u, force, "RX %luL %luB TX %luL", (unsigned long)ui->rx_lines, (unsigned long)ui->rx_bytes,
             (unsigned long)ui->tx_lines);
        rfmt(3u, force, "WD LEFT %5ums", (unsigned)ui->wd_left_ms);
        rfmt(4u, force, "MOTOR ERR 0x%02X", sn->err);
        if (ui->err_code == 0u)
        {
            rfmt(5u, force, "PROTO ERR 0 (none)");
        }
        else
        {
            rfmt(5u, force, "PROTO ERR %u", (unsigned)ui->err_code);
        }
        if (ui->tel_period_ms == 0u)
        {
            rfmt(6u, force, "TEL off");
        }
        else
        {
            rfmt(6u, force, "TEL %ums", (unsigned)ui->tel_period_ms);
        }
        rfmt(7u, force, "#EN #V #P #STOP");
        break;

    /* --------------------------------- P3 SYSTEM --------------------------------- */
    default:
        rfmt(0u, force, "SYSTEM %u/%u UP %lus", (unsigned)(page + 1u), (unsigned)ui->page_cnt, (unsigned long)ui->uptime_s);
        rfmt(1u, force, "FW %s", FW_VERSION_STR);
        rfmt(2u, force, "PROTO v1 %s", "serial");
        rfmt(3u, force, "HEAP %lu B", (unsigned long)ui->free_heap);
        rfmt(4u, force, "TASKS %lu", (unsigned long)ui->task_cnt);
        rfmt(5u, force, "EV 0x%04lX", (unsigned long)(ui->ev_flags & 0xFFFFu));
        rfmt(6u, force, "PMAX %.0f deg", (double)(J8108_P_HI * J8108_RAD2DEG));
        rfmt(7u, force, "KEY: UI ONLY (no motor)");
        break;
    }

    OLED_Update(); /* 变更行已写入显存；此处统一提交（无变化时也只是一次空转 I2C 命令） */
}

uint8_t Oled_SelfTest(void)
{
    uint32_t age;

    if (s_draw_ms == 0u)
        return 2u; /* 还没画过一帧：Monitor 可能没起来 */
    age = HAL_GetTick() - s_draw_ms;
    return (age < 2000u) ? 1u : 2u; /* 2s 没刷新 → 可疑（屏/监视任务停了） */
}

#endif /* FEATURE_DISP_UI */
