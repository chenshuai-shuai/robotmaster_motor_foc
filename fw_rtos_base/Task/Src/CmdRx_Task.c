/*
 * CmdRx_Task.c - 串口命令接收/解析/分派（协议 v1）
 *
 * 协议：docs/协议_串口控制_v1.md（命令 #CMD，回包 @OK/@ERR，事件 @EVT，遥测 @TEL）
 * 设计要点：
 *   1. **中断侧只搬字节**：ISR 里绝不做 snprintf/日志（历史上放过日志导致任务级通知死锁）
 *   2. 环形缓冲 512B，溢出计数并丢弃（任务侧报 @ERR 2），绝不阻塞中断
 *   3. 命令 → 语义映射本任务完成；**CAN 发帧一律交给 J8108 任务**（使能类走异步握手）
 *   4. 任何**合法命令**都续心跳（= "上位机活着"），超时由 J8108 任务退阻尼
 */
#include "CmdRx_Task.h"

#include "FreeRTOS.h"
#include "task.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#include "bsp_log.h"
#include "stack_probe.h" /* 栈余量自报 */
#include "bsp_usart.h"
#include "proto_tx.h"
#include "cmd_parse.h"
#include "ctrl_core.h"
#include "motor_8108.h"
#include "J8108_Task.h"
#include "Monitor_Task.h"
#include "ui_status.h"
#include "Led_Task.h"     /* #ST led  自检 */
#include "disp_port.h"    /* #ST disp 自检：Disp_SelfTest()（屏抽象层，单色/彩屏驱动各自实现） */
#include "bsp_key.h"      /* #ST key  自检 */
#include "version.h"
#include "feature_config.h"

#if FEATURE_SERIAL_CTRL
/* M3 文件级隔离（docs/规范_功能宏与模块化.md R3）：未启用时本文件编译为空对象。
 * 被谁调用必须由调用点用同一个宏保护（忘保护=链接失败，这是刻意设计的 fail-fast）。 */

extern USARTInstance uart10; /* uart10_def.c（USART6） */

#define CMD_RING_SIZE (512u)
#define CMD_LINE_MAX (128u)
#define CMD_POLL_MS (2u)
#define CMD_TASK_PRIORITY (4U)
#define CMD_TASK_STACK_WORDS (640U) /* snprintf(浮点)+快照+b[256] → 实机教训后加大留裕量 */
#define CMD_REQ_WAIT_MS (120u)      /* 使能类握手最长等待 */

/* ------------------------------ 环形缓冲（ISR 写 / 任务读） ------------------------------ */
static volatile uint8_t s_ring[CMD_RING_SIZE];
static volatile uint16_t s_w = 0u;
static volatile uint16_t s_r = 0u;
static volatile uint16_t s_ovf = 0u;
static volatile uint32_t s_rx_bytes = 0u; /* 收到的原始字节数（诊断：区分"没字节"与"有字节没换行"） */

/* 统计（供 UI/遥测） */
static uint32_t s_rx_lines = 0u;
static uint8_t s_rx_logged = 0u; /* 前 3 行原样打印（联调诊断），之后静默 */
static char s_last_cmd[UI_LAST_CMD_LEN];
static uint8_t s_last_err = 0u;

/* ------------------------------ ISR：只搬字节 ------------------------------ */
void CmdRx_RxIsr(void)
{
    uint16_t n = (uint16_t)uart10.recv_len;
    uint16_t i;

    if (n > (uint16_t)USART_RXBUFF_LIMIT)
    {
        n = (uint16_t)USART_RXBUFF_LIMIT;
    }
    s_rx_bytes += (uint32_t)n; /* 字节计数（诊断用：分清"没收到字节"和"收到但没有换行"） */
    for (i = 0u; i < n; i++)
    {
        uint16_t nw = (uint16_t)((s_w + 1u) % CMD_RING_SIZE);

        if (nw == s_r)
        {
            s_ovf++;
            return; /* 环形满：丢弃（任务侧会报 @ERR 2），绝不阻塞中断 */
        }
        s_ring[s_w] = uart10.recv_buff[i];
        s_w = nw;
    }
}

static uint8_t ring_pop(uint8_t *out)
{
    if (s_r == s_w)
    {
        return 0u;
    }
    *out = s_ring[s_r];
    s_r = (uint16_t)((s_r + 1u) % CMD_RING_SIZE);
    return 1u;
}

/* ------------------------------ 回包 ------------------------------ */
static void reply_ok(const char *fmt, ...)
{
    char body[128];
    char line[144];
    va_list ap;

    va_start(ap, fmt);
    (void)vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    (void)snprintf(line, sizeof(line), "@OK %s", body);
    Proto_Send(line);
}

static void reply_err(uint8_t code, const char *cmd, const char *why)
{
    char line[144];

    s_last_err = code;
    (void)snprintf(line, sizeof(line), "@ERR %u %s %s", (unsigned)code, (cmd != NULL) ? cmd : "?", (why != NULL) ? why : "");
    Proto_Send(line);
    LOG_W("cmd", "@ERR %u (%s) %s", (unsigned)code, (cmd != NULL) ? cmd : "?", (why != NULL) ? why : "");
}

/* 取命令名（'#' 后的第一个 token，大写化，供回包/UI） */
static void first_token(const char *line, uint16_t len, char *out, uint16_t outsz)
{
    uint16_t i = 0u;
    uint16_t k = 0u;

    while ((i < len) && ((line[i] == ' ') || (line[i] == '\t')))
        i++;
    if ((i < len) && (line[i] == '#'))
        i++;
    while ((i < len) && (line[i] != ' ') && (line[i] != '\t') && (line[i] != '\r') && (line[i] != '\n') && (k < (outsz - 1u)))
    {
        char c = line[i];
        if ((c >= 'a') && (c <= 'z'))
            c = (char)(c - 32);
        out[k++] = c;
        i++;
    }
    out[k] = '\0';
}

/* --------------------------- #ST 模块自检 --------------------------- */
/*
 * 自检码（回包用）：0=本固件未编译该模块 · 1=OK · 2=WARN（活着但状态可疑） · 3=FAIL
 * 全部**非破坏性**：只读状态/寄存器，不发帧、不写外设、不闪灯。
 * 设计：未编译的模块也占位（返回 0）—— 汇总行永远 6 个键，"少一个键"比"错一个键"更糟。
 */
#define ST_FAIL (3u)

#if FEATURE_DISP_UI
static uint8_t st_disp(void) { return Disp_SelfTest(); }
#else
static uint8_t st_disp(void) { return 0u; } /* 未编译 */
#endif

#if FEATURE_LED_TASK
static uint8_t st_led(void) { return Led_SelfTest(); }
#else
static uint8_t st_led(void) { return 0u; }
#endif

#if FEATURE_KEY
static uint8_t st_key(void) { return Key_SelfTest(); }
#else
static uint8_t st_key(void) { return 0u; }
#endif

#if FEATURE_J8108
static uint8_t st_can(void) { return J8108_CanSelfTest(); }
static uint8_t st_j8108(void) { return J8108_SelfTest(); }
#else
static uint8_t st_can(void) { return 0u; }
static uint8_t st_j8108(void) { return 0u; }
#endif

#if FEATURE_MONITOR_TASK
static uint8_t st_monitor(void) { return Monitor_SelfTest(); }
#else
static uint8_t st_monitor(void) { return 0u; }
#endif

typedef struct
{
    const char *name; /* 显示用小写；用户输入已被解析器转大写，比较时临时转回 */
    uint8_t (*fn)(void);
} St_Entry_t;

static const St_Entry_t s_st_tab[] = {
    { "disp", st_disp },   { "led", st_led },     { "key", st_key },
    { "can", st_can },     { "j8108", st_j8108 }, { "monitor", st_monitor },
};

/* 关键词（已大写）与表内小写名等值比较 */
static uint8_t st_name_eq(const char *upper_in, const char *lower_ref)
{
    uint8_t k = 0u;

    while (lower_ref[k] != '\0')
    {
        char a = upper_in[k];
        char b = lower_ref[k];

        if ((b >= 'a') && (b <= 'z'))
            b = (char)(b - 32);
        if ((a == '\0') || (a != b))
            return 0u;
        k++;
    }
    return (upper_in[k] == '\0') ? 1u : 0u;
}

static void cmd_st(const Cmd_t *c)
{
    const uint8_t n = (uint8_t)(sizeof(s_st_tab) / sizeof(s_st_tab[0]));
    uint8_t i;

    if (c->nw >= 1u)
    {
        /* 单模块查询：#ST <模块> */
        for (i = 0u; i < n; i++)
        {
            if (st_name_eq(c->w[0], s_st_tab[i].name) == 0u)
                continue;
            {
                uint8_t code = s_st_tab[i].fn();
                char why[64];

                if (code == 0u)
                {
                    (void)snprintf(why, sizeof(why), "module=%s code=255 (not compiled in this profile)",
                                   s_st_tab[i].name);
                    reply_err(9, "ST", why);
                }
                else if (code >= ST_FAIL)
                {
                    reply_err(6, "ST", "self-test FAIL");
                }
                else
                {
                    reply_ok("ST %s=%u", s_st_tab[i].name, (unsigned)code);
                }
                return;
            }
        }
        reply_err(2, "ST", "usage: #ST [disp|led|key|can|j8108|monitor]");
        return;
    }

    /* 汇总：一行 6 个模块（≤6 个/行，避开 127B 截断的老坑） */
    {
        char line[128];
        uint16_t off = 0u;
        uint8_t fail = 0u;

        line[0] = '\0';
        for (i = 0u; i < n; i++)
        {
            if (off >= (uint16_t)(sizeof(line) - 24u))
                break; /* 防御：理论上 6 个模块不会到这儿 */
            {
                uint8_t code = s_st_tab[i].fn();

                if (code >= ST_FAIL)
                    fail = 1u;
                off += (uint16_t)snprintf(&line[off], sizeof(line) - off, "%s%s=%u",
                                          (i == 0u) ? "" : " ", s_st_tab[i].name, (unsigned)code);
            }
        }
        if (fail != 0u)
            reply_err(6, "ST", line); /* 有模块 FAIL → 回包本身必须是错误，不能被当成全绿 */
        else
            reply_ok("ST %s", line); /* 必须带命令名：@OK 的载荷一律以 <CMD> 开头（协议 §2 口径统一） */
    }
}

static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

/* ------------------------------ 使能类异步握手 ------------------------------ */
static void do_req(J8108_Req_e req, const char *name)
{
    J8108_Res_e res = J8108_RES_NONE;
    uint16_t waited = 0u;

    J8108_ReqStart(req, "serial");
    while (waited < CMD_REQ_WAIT_MS)
    {
        vTaskDelay(pdMS_TO_TICKS(2));
        waited = (uint16_t)(waited + 2u);
        res = J8108_ReqResult();
        if (res != J8108_RES_NONE)
        {
            break;
        }
    }
    J8108_ReqClear();

    switch (res)
    {
    case J8108_RES_OK:
        if (req == J8108_REQ_EN)
        {
            reply_ok("%s mode=DAMP frames=ON", name);
        }
        else
        {
            reply_ok("%s done", name);
        }
        break;
    case J8108_RES_NOACK:
        reply_err(5, name, "no ACK on bus: check 24V / CAN_H-L / 120R / CANID");
        break;
    case J8108_RES_BUSY:
        reply_err(5, name, "CAN not ready (init fail / bus error)");
        break;
    case J8108_RES_DISABLED:
        /* M2：本固件没编译电机模块 —— 必须明确回"功能未启用"，
         * 不许让上位机拿到误导性的 NOACK（以为是接线/供电问题） */
        reply_err(4, name, "feature=j8108 off (this firmware has no motor module)");
        break;
    default:
        reply_err(8, name, "timeout (control task busy?)");
        break;
    }
}

/* ------------------------------ 参数设置（#SET） ------------------------------ */
static const char *set_one(const char *key, float v)
{
    Ctrl_Params_t *p = J8108_Params();

    if (key == NULL)
        return "missing key";

    if (strcmp(key, "KD_DAMP") == 0)
    {
        if ((v < 0.0f) || (v > 5.0f))
            return "range 0..5";
        p->kd_damp = v;
    }
    else if (strcmp(key, "KP_POS") == 0)
    {
        if ((v < 0.0f) || (v > 500.0f))
            return "range 0..500";
        p->kp_pos = v;
    }
    else if (strcmp(key, "KD_POS") == 0)
    {
        if ((v < 0.0f) || (v > 5.0f))
            return "range 0..5";
        p->kd_pos = v;
    }
    else if (strcmp(key, "KP_V") == 0)
    {
        if ((v < 0.0f) || (v > 10.0f))
            return "range 0..10";
        p->kp_v = v;
    }
    else if (strcmp(key, "KI_V") == 0)
    {
        if ((v < 0.0f) || (v > 1.0f))
            return "range 0..1";
        p->ki_v = v;
    }
    else if (strcmp(key, "KP_IMP") == 0)
    {
        if ((v < 0.0f) || (v > 500.0f))
            return "range 0..500";
        p->kp_imp = v;
    }
    else if (strcmp(key, "KD_IMP") == 0)
    {
        if ((v < 0.0f) || (v > 5.0f))
            return "range 0..5";
        p->kd_imp = v;
    }
    else if (strcmp(key, "INPOS") == 0)
    {
        if ((v < 0.1f) || (v > 90.0f))
            return "range 0.1..90";
        p->inpos_deg = v;
    }
    else if (strcmp(key, "FOLLOW") == 0)
    {
        if ((v < 1.0f) || (v > 720.0f))
            return "range 1..720";
        p->follow_deg = v;
    }
    else if (strcmp(key, "RATE") == 0)
    {
        if ((v < 10.0f) || (v > 100000.0f))
            return "range 10..100000";
        p->rate_dps2 = v;
    }
    else if (strcmp(key, "TFF") == 0)
    {
        /* 摩擦前馈 N·m（0..18）：静摩擦大的关节设成"维持转动所需力矩"即可大幅改善起步 */
        if ((v < 0.0f) || (v > 18.0f))
            return "range 0..18 Nm";
        p->tff_nm = v;
    }
    else if (strcmp(key, "TRATE") == 0)
    {
        /* 力矩斜率上限 N·m/s（0=不限）。小惯量关节建议 5~50；调大=响应快但易震荡 */
        if ((v < 0.0f) || (v > 1000.0f))
            return "range 0..1000 Nm/s";
        p->trate_nm_s = v;
    }
    else if (strcmp(key, "WD") == 0)
    {
        if ((v < 0.0f) || (v > 5000.0f))
            return "range 0..5000";
        p->wd_ms = (uint16_t)v;
    }
    else if (strcmp(key, "VMAX") == 0)
    {
        if ((v < 1.0f) || (v > 4500.0f))
            return "range 1..4500";
        p->vmax_dps = v;
    }
    else if (strcmp(key, "TMAX") == 0)
    {
        if ((v < 0.1f) || (v > 18.0f))
            return "range 0.1..18";
        p->tmax_nm = v;
    }
    else if (strcmp(key, "AUTODAMP") == 0)
    {
        p->autodamp_on_err = (v != 0.0f) ? 1u : 0u;
    }
    else if (strcmp(key, "SETP") == 0)
    {
        p->setp_unlocked = (v != 0.0f) ? 1u : 0u;
    }
    else
    {
        return "unknown key";
    }
    return NULL;
}

/* ------------------------------ 分派 ------------------------------ */
#if J8108_SCALE_OUTPUT_SIDE
#define CMD_SCALE_STR "output"
#else
#define CMD_SCALE_STR "motor"
#endif

static void dispatch(const Cmd_t *c, const char *name)
{
    char b[256]; /* 本地缓冲（协议行另有 127 字节上限，超长行必须拆行：#GET/#HELP 已拆） */
    J8108_Snapshot_t sn;
    const Ctrl_State_t *st = J8108_State();
    Ctrl_Params_t *pp = J8108_Params();

    switch (c->id)
    {
    /* ------------------------------- 系统 ------------------------------- */
    case CMD_PING:
        (void)snprintf(b, sizeof(b), "@OK PING ms=%lu", (unsigned long)now_ms());
        Proto_Send(b);
        break;

    case CMD_VER:
        /* 注意：协议单行 ≤127 字节（上限在 proto_tx），此处刻意精简 */
        (void)snprintf(b, sizeof(b),
                       "@OK VER fw=%s proto=1 u=deg,dps,Nm pmax=%.0f vmax=%.0f tmax=%.0f scale=%s dir=%+.0f canid=0x%02X ctrl=%uHz",
                       FW_VERSION_STR, (double)(J8108_P_HI * J8108_RAD2DEG), (double)J8108_V_HI, (double)J8108_T_HI,
                       CMD_SCALE_STR, (double)J8108_DIR, (unsigned)J8108_CAN_ID, (unsigned)(1000u / 5u));
        Proto_Send(b);
        break;

    case CMD_STAT:
    {
        char agebuf[16];
        uint8_t fresh_stat;

        J8108_CopySnapshot(&sn);
        fresh_stat = ((sn.valid != 0u) && (sn.frame_age_ms < 200u)) ? 1u : 0u;
        if (sn.rx_count == 0u)
        {
            (void)snprintf(agebuf, sizeof(agebuf), "never");
        }
        else
        {
            (void)snprintf(agebuf, sizeof(agebuf), "%lums", (unsigned long)sn.frame_age_ms);
        }
        /* ★ 数据不新鲜时**绝不打印解码数值**：rx=0 时缓冲全 0，会被解算成量程最小值
         *   （-720deg / -2578dps / -18Nm），看起来像真实数据 —— 实机 2026-09-18 踩过这个坑 */
        if (fresh_stat == 0u)
        {
            (void)snprintf(b, sizeof(b),
                           "@OK STAT link=DOWN mode=%s en=%u rx=%lu tx=%lu age=%s hold=%u set=%.2f (values withheld: no fresh frame)",
                           Ctrl_ModeStr(st->mode), (unsigned)st->enabled, (unsigned long)sn.rx_count,
                           (unsigned long)sn.tx_cnt, agebuf, (unsigned)J8108_IsSending(), (double)st->set);
        }
        else
        {
            (void)snprintf(b, sizeof(b),
                           "@OK STAT link=UP mode=%s en=%u pos=%.2f vel=%.1f T=%.3f Tm=%.1f Tr=%.1f err=0x%02X rx=%lu tx=%lu age=%s hold=%u set=%.2f",
                           Ctrl_ModeStr(st->mode), (unsigned)st->enabled, (double)sn.pos_deg, (double)sn.vel_dps,
                           (double)sn.torque, (double)sn.t_mos, (double)sn.t_rotor, sn.err,
                           (unsigned long)sn.rx_count, (unsigned long)sn.tx_cnt, agebuf,
                           (unsigned)J8108_IsSending(), (double)st->set);
        }
        Proto_Send(b);
        break;
    }

    case CMD_LOG:
        if (c->nf < 1u)
        {
            reply_err(2, name, "usage: #LOG <0=err|1=info|2|3=debug>");
            break;
        }
        if ((c->f[0] < 0.0f) || (c->f[0] > 3.0f))
        {
            reply_err(2, name, "level 0..3");
            break;
        }
        {
            uint8_t lv = (uint8_t)c->f[0];
            log_set_level((lv == 0u) ? LOG_LEVEL_ERROR : ((lv == 1u) ? LOG_LEVEL_INFO : LOG_LEVEL_DEBUG));
            reply_ok("LOG lvl=%u (0=err 1=info 2/3=debug)", (unsigned)lv);
        }
        break;

    case CMD_CLR:
        do_req(J8108_REQ_CLRERR, "CLR"); /* 走控制任务发 0x001…FA（本任务不碰 CAN） */
        break;

    case CMD_SET:
        if ((c->nf < 1u) || (c->nw < 1u))
        {
            reply_err(2, name, "usage: #SET <key> <value>  (see #GET)");
            break;
        }
        {
            const char *bad = set_one(c->w[0], c->f[0]);
            if (bad != NULL)
            {
                reply_err(2, name, bad);
            }
            else
            {
                reply_ok("SET %s=%.3f", c->w[0], (double)c->f[0]);
            }
        }
        break;

    case CMD_GET:
        /* 参数较多 → 拆 3 行（协议单行 ≤127 字节） */
        (void)snprintf(b, sizeof(b), "@OK GET gains kd_damp=%.3f kp_pos=%.1f kd_pos=%.2f kp_v=%.3f ki_v=%.4f tff=%.3f",
                       (double)pp->kd_damp, (double)pp->kp_pos, (double)pp->kd_pos, (double)pp->kp_v, (double)pp->ki_v,
                       (double)pp->tff_nm);
        Proto_Send(b);
        (void)snprintf(b, sizeof(b), "@OK GET limits pmin=%.1f pmax=%.1f vmax=%.0f tmax=%.2f rate=%.0f trate=%.0f wd=%u",
                       (double)pp->pmin_deg, (double)pp->pmax_deg, (double)pp->vmax_dps, (double)pp->tmax_nm,
                       (double)pp->rate_dps2, (double)pp->trate_nm_s, (unsigned)pp->wd_ms);
        Proto_Send(b);
        (void)snprintf(b, sizeof(b), "@OK GET misc inpos=%.2f follow=%.1f stall=%.1f/%.1fdps autodamp=%u setp=%u",
                       (double)pp->inpos_deg, (double)pp->follow_deg, (double)pp->stall_nm, (double)pp->stall_dps,
                       (unsigned)pp->autodamp_on_err, (unsigned)pp->setp_unlocked);
        Proto_Send(b);
        break;

    /* --------------------------- 使能 / 安全 --------------------------- */
    case CMD_EN:
        do_req(J8108_REQ_EN, "EN");
        break;
    case CMD_DIS:
        do_req(J8108_REQ_DIS, "DIS");
        break;
    case CMD_ZERO:
        /* 零点会**掉电持久保存**（可能改变机械零点语义）→ 要求二次确认（协议 §5.2） */
        if (cmd_word_is(c, 0u, "CONFIRM") == 0u)
        {
            reply_err(4, name, "persistent! resend as `#ZERO CONFIRM` (writes motor zero, survives power-off)");
            break;
        }
        do_req(J8108_REQ_ZERO, "ZERO");
        break;

    case CMD_STOP: /* 软急停：阻尼（保持使能，防坠） */
        J8108_StopSoft();
        Proto_Send("@OK STOP mode=DAMP (still enabled)");
        Proto_Send("@EVT STOP mode=DAMP");
        LOG_W("cmd", "STOP: soft e-stop -> DAMP (motor stays enabled)");
        break;

    case CMD_ESTOP: /* 硬急停：停帧 + 失能 */
        J8108_StopHard();
        Proto_Send("@OK ESTOP mode=DISABLED (shaft FREE)");
        break;

    /* ------------------------------- 运动 ------------------------------- */
    case CMD_MODE:
        if (c->nw < 1u)
        {
            reply_err(2, name, "usage: #MODE <IDLE|DAMP|SPEED|POS|TORQUE|IMP>");
            break;
        }
        {
            Ctrl_Mode_e m;
            uint8_t known = 1u;

            if (cmd_word_is(c, 0u, "IDLE") != 0u)
                m = CTRL_MODE_IDLE;
            else if (cmd_word_is(c, 0u, "DAMP") != 0u)
                m = CTRL_MODE_DAMP;
            else if (cmd_word_is(c, 0u, "SPEED") != 0u)
                m = CTRL_MODE_SPEED;
            else if (cmd_word_is(c, 0u, "POS") != 0u)
                m = CTRL_MODE_POS;
            else if (cmd_word_is(c, 0u, "TORQUE") != 0u)
                m = CTRL_MODE_TORQUE;
            else if (cmd_word_is(c, 0u, "IMP") != 0u)
                m = CTRL_MODE_IMP;
            else
            {
                m = CTRL_MODE_IDLE;
                known = 0u;
            }
            if (known == 0u)
            {
                reply_err(2, name, "unknown mode");
                break;
            }
            if (J8108_SetMode(m) == 0u)
            {
                reply_err(3, name, "not enabled (send #EN first)");
                break;
            }
            reply_ok("MODE %s", Ctrl_ModeStr(m));
            (void)snprintf(b, sizeof(b), "@EVT MODE=%s", Ctrl_ModeStr(m));
            Proto_Send(b);
        }
        break;

    case CMD_DAMP:
        J8108_SetDampKd((c->nf >= 1u) ? c->f[0] : pp->kd_damp);
        reply_ok("DAMP kd=%.2f", (double)J8108_Params()->kd_damp);
        break;

    case CMD_V: /* 指定速度旋转 */
        if (c->nf < 1u)
        {
            reply_err(2, name, "usage: #V <deg/s> [kd] [tff]");
            break;
        }
        if (J8108_IsEnabled() == 0u)
        {
            reply_err(3, name, "not enabled (send #EN first)");
            break;
        }
        if (c->nf >= 2u)
        {
            const char *bad = set_one("KD_DAMP", c->f[1]);
            if (bad != NULL)
            {
                reply_err(2, name, bad);
                break;
            }
        }
        J8108_SetVelDps(c->f[0]);
        if (c->nf >= 3u)
        {
            J8108_SetVelTff(c->f[2]); /* 前馈（N·m）：不切模式，只加进速度环输出 */
        }
        reply_ok("V target=%.2f dps mode=SPEED", (double)c->f[0]);
        break;

    case CMD_P: /* 绝对位置（纯 PD） */
        if (c->nf < 1u)
        {
            reply_err(2, name, "usage: #P <deg> [kp] [kd]");
            break;
        }
        if (J8108_IsEnabled() == 0u)
        {
            reply_err(3, name, "not enabled (send #EN first)");
            break;
        }
        if (c->nf >= 2u)
        {
            const char *bad = set_one("KP_POS", c->f[1]);
            if (bad != NULL)
            {
                reply_err(2, name, bad);
                break;
            }
        }
        if (c->nf >= 3u)
        {
            const char *bad = set_one("KD_POS", c->f[2]);
            if (bad != NULL)
            {
                reply_err(2, name, bad);
                break;
            }
        }
        if ((c->f[0] < pp->pmin_deg) || (c->f[0] > pp->pmax_deg))
        {
            reply_err(7, name, "outside soft limits (see #LIM)");
            break;
        }
        J8108_SetPosDeg(c->f[0]);
        reply_ok("P target=%.2f deg mode=POS kp=%.1f kd=%.2f", (double)c->f[0], (double)pp->kp_pos, (double)pp->kd_pos);
        break;

    case CMD_T: /* 力矩 */
        if (c->nf < 1u)
        {
            reply_err(2, name, "usage: #T <Nm>");
            break;
        }
        if (J8108_IsEnabled() == 0u)
        {
            reply_err(3, name, "not enabled (send #EN first)");
            break;
        }
        if ((c->f[0] > pp->tmax_nm) || (c->f[0] < -pp->tmax_nm))
        {
            reply_err(2, name, "exceeds #LIM T (torque clamp)");
            break;
        }
        J8108_SetTorqueNm(c->f[0]);
        reply_ok("T target=%.3f Nm mode=TORQUE (no load => keeps accelerating, watch speed)", (double)c->f[0]);
        break;

    case CMD_IMP: /* 阻抗 */
        if (c->nf < 2u)
        {
            reply_err(2, name, "usage: #IMP <kp> <kd> [tff]");
            break;
        }
        if (J8108_IsEnabled() == 0u)
        {
            reply_err(3, name, "not enabled (send #EN first)");
            break;
        }
        J8108_SetImp(c->f[0], c->f[1], (c->nf >= 3u) ? c->f[2] : 0.0f);
        reply_ok("IMP kp=%.1f kd=%.2f tff=%.3f mode=IMP", (double)pp->kp_imp, (double)pp->kd_imp,
                 (double)((c->nf >= 3u) ? c->f[2] : 0.0f));
        break;

    case CMD_HOLD:
        (void)J8108_SetMode(CTRL_MODE_DAMP);
        reply_ok("HOLD mode=DAMP");
        break;

    case CMD_SETP: /* 原始 MIT 直通（默认锁死） */
        if (pp->setp_unlocked == 0u)
        {
            reply_err(4, name, "locked: use `#SET SETP 1` to unlock (raw motor units, no clamping)");
            break;
        }
        if (c->nf < 5u)
        {
            reply_err(2, name, "usage: #SETP <p_rad> <v_rps> <kp> <kd> <t_nm>");
            break;
        }
        if (J8108_IsEnabled() == 0u)
        {
            reply_err(3, name, "not enabled (send #EN first)");
            break;
        }
        J8108_SetRawMIT(c->f[0], c->f[1], c->f[2], c->f[3], c->f[4]);
        reply_ok("SETP raw p=%.4f v=%.3f kp=%.1f kd=%.2f t=%.3f (motor units, unclamped)", (double)c->f[0],
                 (double)c->f[1], (double)c->f[2], (double)c->f[3], (double)c->f[4]);
        break;

    /* --------------------------- 限制 / 心跳 --------------------------- */
    case CMD_LIM:
        if (c->nw == 0u)
        {
            (void)snprintf(b, sizeof(b), "@OK LIM pmin=%.1f pmax=%.1f vmax=%.0f tmax=%.2f rate=%.0f trate=%.0f wd=%u",
                           (double)pp->pmin_deg, (double)pp->pmax_deg, (double)pp->vmax_dps, (double)pp->tmax_nm,
                           (double)pp->rate_dps2, (double)pp->trate_nm_s, (unsigned)pp->wd_ms);
            Proto_Send(b);
        }
        else if (cmd_word_is(c, 0u, "P") != 0u)
        {
            if (c->nf < 2u)
            {
                reply_err(2, name, "usage: #LIM P <min_deg> <max_deg>");
                break;
            }
            if (c->f[0] >= c->f[1])
            {
                reply_err(2, name, "min must be < max");
                break;
            }
            if ((c->f[0] < -J8108_P_HI * J8108_RAD2DEG) || (c->f[1] > J8108_P_HI * J8108_RAD2DEG))
            {
                reply_err(2, name, "outside motor range (pmax from #VER)");
                break;
            }
            pp->pmin_deg = c->f[0];
            pp->pmax_deg = c->f[1];
            reply_ok("LIM P min=%.1f max=%.1f", (double)pp->pmin_deg, (double)pp->pmax_deg);
        }
        else if (cmd_word_is(c, 0u, "V") != 0u)
        {
            if (c->nf < 1u)
            {
                reply_err(2, name, "usage: #LIM V <max_dps>");
                break;
            }
            if ((c->f[0] < 1.0f) || (c->f[0] > J8108_V_HI * J8108_RAD2DEG))
            {
                reply_err(2, name, "outside motor range");
                break;
            }
            pp->vmax_dps = c->f[0];
            reply_ok("LIM V max=%.1f dps", (double)pp->vmax_dps);
        }
        else if (cmd_word_is(c, 0u, "T") != 0u)
        {
            if (c->nf < 1u)
            {
                reply_err(2, name, "usage: #LIM T <max_nm>");
                break;
            }
            if ((c->f[0] < 0.1f) || (c->f[0] > J8108_T_HI))
            {
                reply_err(2, name, "outside motor range (0.1..18)");
                break;
            }
            pp->tmax_nm = c->f[0];
            reply_ok("LIM T max=%.2f Nm", (double)pp->tmax_nm);
        }
        else
        {
            reply_err(2, name, "usage: #LIM [P min max | V max | T max]");
        }
        break;

    case CMD_RATE:
        if (c->nf < 1u)
        {
            reply_err(2, name, "usage: #RATE <deg/s^2>");
            break;
        }
        {
            const char *bad = set_one("RATE", c->f[0]);
            if (bad != NULL)
            {
                reply_err(2, name, bad);
            }
            else
            {
                reply_ok("RATE %.0f dps2", (double)pp->rate_dps2);
            }
        }
        break;

    case CMD_WD: /* 心跳超时（0=关） */
        if (c->nf < 1u)
        {
            reply_err(2, name, "usage: #WD <ms>  (0=off)");
            break;
        }
        {
            const char *bad = set_one("WD", c->f[0]);
            if (bad != NULL)
            {
                reply_err(2, name, bad);
            }
            else
            {
                reply_ok("WD %ums -> DAMP on timeout", (unsigned)pp->wd_ms);
            }
        }
        break;

    /* ------------------------------- 遥测 ------------------------------- */
    case CMD_TEL:
#if FEATURE_MONITOR_TASK
        if (c->nf < 1u)
        {
            reply_err(2, name, "usage: #TEL <0=off | 1=once | period_ms 2..1000>");
            break;
        }
        if (c->f[0] < 1.0f)
        {
            Monitor_SetTelPeriod(0u);
            reply_ok("TEL period=0 (off)");
        }
        else if (c->f[0] == 1.0f)
        {
            Monitor_FormatTel(b, sizeof(b));
            Proto_Send(b);
            reply_ok("TEL once");
        }
        else if (c->f[0] > 1000.0f)
        {
            reply_err(2, name, "period 2..1000 ms (115200 baud limit)");
        }
        else
        {
            uint16_t ms = (uint16_t)c->f[0];
            Monitor_SetTelPeriod(ms);
            reply_ok("TEL period=%ums", (unsigned)ms);
        }
#else
        /* M2：监控任务未编译 → 不许回 @OK（否则"回包与行为不一致"，本工程的老毛病） */
        reply_err(4, name, "feature=monitor off (telemetry has no producer)");
#endif
        break;

    case CMD_ST:
        /* 模块自检（分块调试入口）：#ST 或 #ST <模块> —— 见 docs/操作手册_串口命令.md */
        cmd_st(c);
        break;

    case CMD_HELP:
        Proto_Send("@OK HELP cmd=PING,VER,STAT,ST,LOG,CLR,SET,GET,EN,DIS,STOP,ESTOP,ZERO,MODE");
        Proto_Send("@OK HELP cmd2=DAMP,V,P,T,IMP,HOLD,SETP,LIM,RATE,WD,TEL,HELP");
        Proto_Send("@OK HELP units=deg,dps,Nm (joint side) | prefix #cmd -> @OK/@ERR/@EVT/@TEL");
        break;

    default:
        reply_err(1, name, "unknown command");
        break;
    }
}

/* ------------------------------ 任务主体 ------------------------------ */
static void cmdrx_task(void *arg)
{
    char line[CMD_LINE_MAX + 1u];
    uint16_t len = 0u;
    uint8_t ch;
    uint16_t ovf_seen = 0u;

    (void)arg;

    LOG_I("cmd", "serial protocol v1 listener up: #CMD -> @OK/@ERR (log lines are separate)");

    for (;;)
    {
        stack_probe_tick(now_ms(), "cmd", CMD_TASK_STACK_WORDS); /* 栈余量自报（5s 一次） */

        while (ring_pop(&ch) != 0u)
        {
            /* 行结束符：\n 或 \r 都接受（有的上位机只发 CR；CRLF 时第二字符遇 len==0 自动跳过） */
            if ((ch == '\n') || (ch == '\r'))
            {
                if (len > 0u)
                {
                    line[len] = '\0';
                    if (s_rx_logged < 3u)
                    {
                        /* 前 3 行原样打出来（联调诊断用：能看到"到底收到了什么"），之后不再打防刷屏 */
                        s_rx_logged++;
                        LOG_I("cmd", "rx line %u: \"%s\"", (unsigned)s_rx_logged, line);
                    }
                    {
                        Cmd_t cmd;
                        Cmd_ParseRes_e r = Cmd_ParseLine(line, len, &cmd);
                        char name[16];

                        first_token(line, len, name, sizeof(name));
                        if (r == CMD_PARSE_NOTCMD)
                        {
                            /* 非命令：静默（可能是上位机误发/回显） */
                        }
                        else
                        {
                            s_rx_lines++;
                            (void)snprintf(s_last_cmd, sizeof(s_last_cmd), "%s", name);
                            if (r == CMD_PARSE_UNKNOWN)
                            {
                                reply_err(1, name, "unknown command (try #HELP)");
                            }
                            else if (r == CMD_PARSE_BADARG)
                            {
                                reply_err(2, name, "bad arguments");
                            }
                            else
                            {
                                J8108_KeepAlive(); /* 合法命令 = 上位机活着 */
                                dispatch(&cmd, name);
                            }
                        }
                    }
                    len = 0u;
                }
                continue;
            }
            if (len < CMD_LINE_MAX)
            {
                line[len] = (char)ch;
                len++;
            }
            else
            {
                len = 0u; /* 超长行丢弃 */
                reply_err(2, "?", "line too long (max 128 bytes)");
            }
        }

        if (s_ovf != ovf_seen)
        {
            ovf_seen = s_ovf;
            reply_err(2, "?", "rx overflow (host sent too fast)");
        }

        vTaskDelay(pdMS_TO_TICKS(CMD_POLL_MS));
    }
}

void CmdRx_Task_Init(void)
{
    BaseType_t ok = xTaskCreate(cmdrx_task, "CmdRx", CMD_TASK_STACK_WORDS, NULL, CMD_TASK_PRIORITY, NULL);

    if (ok != pdPASS)
    {
        LOG_E("cmd", "xTaskCreate FAILED (heap/stack?) -> SERIAL PROTOCOL DISABLED");
    }
}

uint32_t CmdRx_RxLines(void)
{
    return s_rx_lines;
}

uint32_t CmdRx_RxBytes(void)
{
    return s_rx_bytes;
}

const char *CmdRx_LastCmd(void)
{
    return s_last_cmd;
}

uint8_t CmdRx_LastErr(void)
{
    return s_last_err;
}

#endif /* FEATURE_SERIAL_CTRL */
