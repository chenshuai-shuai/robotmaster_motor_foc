/*
 * Oled_Task.c - 4 页监护界面渲染器（**页面层**：只认 disp_port.h 的 Disp_*，与具体屏无关）
 *
 * 屏：feature_config.h §4 二选一（SH1106 单色 128x64 软 I2C / ST7735S 彩屏 128x160 SPI）；
 *     列数 / 行高 / 行数由 Disp_Info() 算出 —— 同一份代码两种屏都能排
 *     （128x64 单色屏上就是原来的 21 列 x 8 行，逐字不变）。
 * 页面（docs/协议_串口控制_v1.md §13）：
 *   P0 LINK   : CAN 四态 + rx/tx + 回传 Hz + ERR 报错位 + 总线诊断
 *   P1 MOTION : 模式/使能 + pos/vel/T + 设定点 + 本帧 Kp/Kd + 双温度 + 到位
 *   P2 SERIAL : 协议态（最近命令 / 行计数 / 心跳剩余 / 电机报错 / 协议错误 / 遥测周期）
 *   P3 SYSTEM : 版本 / 运行时间 / free heap / 任务数 / 保护事件 / 位置量程
 *
 * 设计（S1 只换"怎么上屏"，页面内容与改造前逐字一致）：
 *   · **局部刷新**：每行与上次比较（文本 + 颜色），只有变化才重画；上屏统一走 Disp_Flush()
 *   · 单色屏把"非黑"当点亮 → 同一套配色在 SH1106 上看到的画面与改造前完全一样，
 *     在彩屏上则按"正常白 / UP-INPOS 绿 / WARN 黄 / ERR-断链-急停 红 / 提示灰"着色
 *   · 本文件**不发任何 CAN 帧**、不碰控制状态（只读快照 / 只读状态视图）
 *   · 只用 ASCII（汉字要等 S4 字库落地，届时 has_cn 打开再换标签）
 *   · **白名单**：本文件不许出现 OLED_ / HAL_GPIO / SPI1 / hspi 等驱动私有符号
 *     （tools/verify_dev.py 断言 V2 —— 换屏不改页面靠机制保证，不靠自觉）
 */
#include "Oled_Task.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#include "disp_port.h"
#include "main.h" /* hcan1 / CAN_ESR_* 宏（P0 的总线诊断行） */
#include "bsp_log.h"
#include "ctrl_core.h"
#include "version.h"
#include "feature_config.h" /* M3：文件级隔离需要一个统一的开关 */

#if FEATURE_DISP_UI
/* M3 文件级隔离（docs/规范_功能宏与模块化.md R3）：未启用时本文件编译为空对象。
 * 被谁调用必须由调用点用同一个宏保护（忘保护=链接失败，这是刻意设计的 fail-fast）。 */

#define UI_COLS_MAX (32u) /* s_last 的编译期上界（32 列 x 24 行 = 768B，静态数组） */
#define UI_ROWS_MAX (24u)

static char s_last[UI_ROWS_MAX][UI_COLS_MAX + 1u];
static uint16_t s_last_fg[UI_ROWS_MAX]; /* 颜色也算"变化"：文本没变但颜色变了要重画 */
static uint8_t s_cols = 21u;            /* 活动列数 = w / font_w[SMALL] */
static uint8_t s_rows = 8u;             /* 活动行数 = h / font_h[SMALL] */
static uint8_t s_line_h = 8u;
static uint8_t s_page = 0xFFu; /* 0xFF = 未绘制过（强制全刷） */
static uint8_t s_layout_ok;

/* 一帧的绘制上下文（省得每个页面函数都带一长串参数） */
typedef struct
{
    uint8_t force; /* 1=强制重画（翻页后） */
    uint8_t fresh; /* 1=反馈帧新鲜（<200ms）—— 不新鲜绝不显示旧数值 */
    const J8108_Snapshot_t *sn;
    const Ui_Status_t *ui;
} Ui_Ctx_t;

/* 版面：由屏能力算出（128x64 单色 → 21 列 x 8 行 x 行高 8px，与改造前一致） */
static void ui_layout(void)
{
    const Disp_Info_t *di = Disp_Info();
    uint16_t cols;
    uint16_t rows;

    if ((di == NULL) || (di->w == 0u) || (di->font_w[DISP_FONT_SMALL] == 0u) ||
        (di->font_h[DISP_FONT_SMALL] == 0u))
    {
        s_layout_ok = 0u; /* 屏坏了 / 没初始化 → 本层全部空操作，系统照跑 */
        return;
    }
    cols = (uint16_t)(di->w / di->font_w[DISP_FONT_SMALL]);
    rows = (uint16_t)(di->h / di->font_h[DISP_FONT_SMALL]);
    if (cols > UI_COLS_MAX)
    {
        cols = UI_COLS_MAX;
    }
    if (rows > UI_ROWS_MAX)
    {
        rows = UI_ROWS_MAX;
    }
    s_cols = (uint8_t)cols;
    s_rows = (uint8_t)rows;
    s_line_h = di->font_h[DISP_FONT_SMALL];
    s_layout_ok = 1u;
}

/* 画一行：格式化 → 补空格到 s_cols → 与上次比较（文本 + 颜色）→ 变化才写屏 */
static void rfmt(uint8_t row, uint8_t force, uint16_t fg, const char *fmt, ...)
{
    char tmp[64];
    char out[UI_COLS_MAX + 1u];
    va_list ap;
    uint8_t n;
    uint8_t i;

    if ((row >= s_rows) || (s_layout_ok == 0u))
    {
        return;
    }

    va_start(ap, fmt);
    (void)vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);

    n = (uint8_t)strlen(tmp);
    if (n > s_cols)
    {
        n = s_cols;
    }
    (void)memcpy(out, tmp, n);
    for (i = n; i < s_cols; i++)
    {
        out[i] = ' ';
    }
    out[s_cols] = '\0';

    if ((force == 0) && (s_last_fg[row] == fg) && (memcmp(out, s_last[row], s_cols) == 0))
    {
        return; /* 无变化：不刷 */
    }
    (void)memcpy(s_last[row], out, s_cols);
    s_last_fg[row] = fg;
    Disp_Text(0, (int16_t)((int16_t)row * (int16_t)s_line_h), out, fg, DISP_C_BG, DISP_FONT_SMALL);
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

/* 配色（彩屏才看得见；单色屏一律"非黑=亮"，所以对 SH1106 的回归无影响） */
static uint16_t col_bus(uint8_t status)
{
    switch (status)
    {
    case J8108_ST_READY:
        return DISP_C_OK;
    case J8108_ST_INIT_FAIL:
    case J8108_ST_BUS_ERR:
        return DISP_C_ERR;
    case J8108_ST_INIT_OK:
        return DISP_C_WARN;
    default:
        return DISP_C_DIM;
    }
}

/* ---------------------------------- P0 LINK ---------------------------------- */
static void ui_page_link(const Ui_Ctx_t *c)
{
    const J8108_Snapshot_t *sn = c->sn;
    const Ui_Status_t *ui = c->ui;

    rfmt(0u, c->force, col_bus(sn->status), "LINK %u/%u %s", (unsigned)(ui->page + 1u),
         (unsigned)ui->page_cnt, bus_state_str(sn->status));
    rfmt(1u, c->force, DISP_C_FG, "RX %lu TX %lu", (unsigned long)sn->rx_count, (unsigned long)sn->tx_cnt);

    if (sn->err == 0u)
    {
        rfmt(2u, c->force, DISP_C_OK, "ERR OK (%s)", J8108_ErrStr(sn->err));
    }
    else
    {
        rfmt(2u, c->force, DISP_C_ERR, "ERR 0x%02X %s", sn->err, J8108_ErrStr(sn->err));
    }

    if (sn->bus_err != 0u)
    {
        uint32_t esr = hcan1.Instance->ESR;

        rfmt(3u, c->force, DISP_C_WARN, "TEC%3lu REC%3lu LEC%lu",
             (unsigned long)((esr & CAN_ESR_TEC) >> CAN_ESR_TEC_Pos),
             (unsigned long)((esr & CAN_ESR_REC) >> CAN_ESR_REC_Pos),
             (unsigned long)((esr & CAN_ESR_LEC) >> CAN_ESR_LEC_Pos));
    }
    else
    {
        rfmt(3u, c->force, DISP_C_DIM, "BUS OK (HAL 0x%04lX)", (unsigned long)(sn->bus_err & 0xFFFFu));
    }

    if (sn->rx_count == 0u)
    {
        rfmt(4u, c->force, DISP_C_ERR, "AGE never  LINK DOWN"); /* 从未收到过反馈帧（不是"年龄 0ms"） */
    }
    else if (c->fresh != 0u)
    {
        rfmt(4u, c->force, DISP_C_OK, "AGE %lums  %uHz", (unsigned long)sn->frame_age_ms, (unsigned)ui->fb_hz);
    }
    else
    {
        rfmt(4u, c->force, DISP_C_ERR, "AGE %lums  LINK DOWN", (unsigned long)sn->frame_age_ms);
    }

    rfmt(5u, c->force, DISP_C_FG, "MODE %s EN %u", Ctrl_ModeStr((Ctrl_Mode_e)ui->mode), (unsigned)ui->enabled);
    rfmt(6u, c->force, DISP_C_FG, "SET %8.2f %s", (double)ui->set_deg, "deg");
    rfmt(7u, c->force, DISP_C_DIM, "KEY CLK=next DBL=prev");
}

/* --------------------------------- P1 MOTION --------------------------------- */
static void ui_page_motion(const Ui_Ctx_t *c)
{
    const J8108_Snapshot_t *sn = c->sn;
    const Ui_Status_t *ui = c->ui;
    uint16_t tcol = ((ui->ev_flags & CTRL_EV_TEMP) != 0u) ? DISP_C_WARN : DISP_C_DIM;
    uint16_t scol = ((ui->ev_flags & CTRL_EV_LIMIT) != 0u) ? DISP_C_WARN : DISP_C_FG;

    rfmt(0u, c->force, DISP_C_FG, "MOTION %u/%u %s", (unsigned)(ui->page + 1u), (unsigned)ui->page_cnt,
         Ctrl_ModeStr((Ctrl_Mode_e)ui->mode));

    if (c->fresh != 0u)
    {
        rfmt(1u, c->force, DISP_C_FG, "POS %9.2f deg", (double)sn->pos_deg);
        rfmt(2u, c->force, DISP_C_FG, "VEL %9.2f dps", (double)sn->vel_dps);
        rfmt(3u, c->force, DISP_C_FG, "T   %9.3f Nm", (double)sn->torque);
        rfmt(6u, c->force, tcol, "Tm %4.1fC Tr %4.1fC", (double)sn->t_mos, (double)sn->t_rotor);
    }
    else
    {
        rfmt(1u, c->force, DISP_C_DIM, "POS      ---- deg"); /* 门控：不新鲜绝不显示旧数值 */
        rfmt(2u, c->force, DISP_C_DIM, "VEL      ---- dps");
        rfmt(3u, c->force, DISP_C_DIM, "T        ---- Nm");
        rfmt(6u, c->force, DISP_C_DIM, "Tm   --.-C Tr  --.-C");
    }

    switch ((Ctrl_Mode_e)ui->mode)
    {
    case CTRL_MODE_POS:
        rfmt(4u, c->force, scol, "SET %9.2f deg", (double)ui->set_deg);
        break;
    case CTRL_MODE_SPEED:
        rfmt(4u, c->force, scol, "SET %9.2f dps", (double)ui->set_dps);
        break;
    case CTRL_MODE_TORQUE:
    case CTRL_MODE_IMP:
        rfmt(4u, c->force, scol, "SET %9.3f Nm", (double)ui->set_nm);
        break;
    default:
        rfmt(4u, c->force, DISP_C_DIM, "SET      ---- ");
        break;
    }

    rfmt(5u, c->force, DISP_C_DIM, "KP %5.1f KD %5.2f", (double)ui->kp_now, (double)ui->kd_now);
    rfmt(7u, c->force, (ui->inpos != 0u) ? DISP_C_OK : DISP_C_DIM, "EN %u SEND %u INPOS %u",
         (unsigned)ui->enabled, (unsigned)ui->frames_on, (unsigned)ui->inpos);
}

/* --------------------------------- P2 SERIAL --------------------------------- */
static void ui_page_serial(const Ui_Ctx_t *c)
{
    const J8108_Snapshot_t *sn = c->sn;
    const Ui_Status_t *ui = c->ui;

    rfmt(0u, c->force, DISP_C_FG, "SERIAL %u/%u", (unsigned)(ui->page + 1u), (unsigned)ui->page_cnt);
    rfmt(1u, c->force, DISP_C_FG, "LAST #%s", (ui->last_cmd[0] != '\0') ? ui->last_cmd : "-");
    rfmt(2u, c->force, DISP_C_DIM, "RX %luL %luB TX %luL", (unsigned long)ui->rx_lines,
         (unsigned long)ui->rx_bytes, (unsigned long)ui->tx_lines);
    rfmt(3u, c->force, DISP_C_DIM, "WD LEFT %5ums", (unsigned)ui->wd_left_ms);
    rfmt(4u, c->force, (sn->err == 0u) ? DISP_C_OK : DISP_C_ERR, "MOTOR ERR 0x%02X", sn->err);

    if (ui->err_code == 0u)
    {
        rfmt(5u, c->force, DISP_C_OK, "PROTO ERR 0 (none)");
    }
    else
    {
        rfmt(5u, c->force, DISP_C_WARN, "PROTO ERR %u", (unsigned)ui->err_code);
    }

    if (ui->tel_period_ms == 0u)
    {
        rfmt(6u, c->force, DISP_C_DIM, "TEL off");
    }
    else
    {
        rfmt(6u, c->force, DISP_C_DIM, "TEL %ums", (unsigned)ui->tel_period_ms);
    }
    rfmt(7u, c->force, DISP_C_DIM, "#EN #V #P #STOP");
}

/* --------------------------------- P3 SYSTEM --------------------------------- */
static void ui_page_system(const Ui_Ctx_t *c)
{
    const Ui_Status_t *ui = c->ui;

    rfmt(0u, c->force, DISP_C_FG, "SYSTEM %u/%u UP %lus", (unsigned)(ui->page + 1u), (unsigned)ui->page_cnt,
         (unsigned long)ui->uptime_s);
    rfmt(1u, c->force, DISP_C_OK, "FW %s", FW_VERSION_STR);
    rfmt(2u, c->force, DISP_C_DIM, "PROTO v1 %s", "serial");
    rfmt(3u, c->force, DISP_C_FG, "HEAP %lu B", (unsigned long)ui->free_heap);
    rfmt(4u, c->force, DISP_C_FG, "TASKS %lu", (unsigned long)ui->task_cnt);
    rfmt(5u, c->force, (ui->ev_flags != 0u) ? DISP_C_ERR : DISP_C_DIM, "EV 0x%04lX",
         (unsigned long)(ui->ev_flags & 0xFFFFu));
    rfmt(6u, c->force, DISP_C_FG, "PMAX %.0f deg", (double)(J8108_P_HI * J8108_RAD2DEG));
    rfmt(7u, c->force, DISP_C_DIM, "KEY: UI ONLY (no motor)");
}

void Oled_UiDraw(uint8_t page, const J8108_Snapshot_t *sn, const Ui_Status_t *ui)
{
    Ui_Ctx_t ctx;
    uint8_t force;

    if ((sn == NULL) || (ui == NULL))
    {
        return;
    }
    if (page >= UI_PAGE_COUNT)
    {
        page = 0u;
    }

    if (s_layout_ok == 0u)
    {
        ui_layout(); /* 首次（或上次失败）：从屏能力算版面；算不出来 → 本层空操作 */
    }
    if (s_layout_ok == 0u)
    {
        return;
    }

    force = (page != s_page) ? 1u : 0u;
    if (force != 0u)
    {
        s_page = page;
        Disp_Clear(DISP_C_BG);
        (void)memset(s_last, 0, sizeof(s_last)); /* 清空缓存 → 全刷 */
        (void)memset(s_last_fg, 0, sizeof(s_last_fg));
    }

    ctx.force = force;
    ctx.fresh = ((sn->valid != 0u) && (sn->frame_age_ms < 200u)) ? 1u : 0u;
    ctx.sn = sn;
    ctx.ui = ui;

    switch (page)
    {
    case UI_PAGE_LINK:
        ui_page_link(&ctx);
        break;
    case UI_PAGE_MOTION:
        ui_page_motion(&ctx);
        break;
    case UI_PAGE_SERIAL:
        ui_page_serial(&ctx);
        break;
    default:
        ui_page_system(&ctx);
        break;
    }

    Disp_Flush(); /* 变更行已写入显存；此处统一提交（无变化时只是一次空转） */
}

#endif /* FEATURE_DISP_UI */
