/*
 * motor_8108.c - 8108 关节电机 CAN 驱动实现（协议说明见 motor_8108.h 头部）
 *
 * 设计要点：
 *   1. 发送走裸 HAL（带自旋保护）——命令帧(0x01)与 MIT 帧(0x201)两个 ID 自由切换；
 *      总线上无应答时（未接电机/未上电）最多自旋若干次后丢弃本帧，不阻塞任务。
 *   2. 接收走项目 CAN 框架（CANRegister）：反馈帧 0x781 到达 → 中断回调仅做
 *      原始数据拷贝+计数（不做浮点/日志），物理量在 J8108_Update() 由任务解算。
 */
#include "motor_8108.h"
#include "string.h"
#include "FreeRTOS.h"
#include "task.h"
#include "bsp_log.h"

static J8108_t s_dev;
static uint8_t s_inited = 0;

/* ------------------------------- 发送（带自旋保护） ------------------------------- */
static void j8108_tx(uint32_t std_id, const uint8_t *data)
{
    CAN_TxHeaderTypeDef hdr;
    uint32_t mailbox;
    uint32_t guard = 0;

    if (!s_inited)
        return;

    /* ★ 自旋保护要"短"：本函数在 **prio 5 的控制任务**里每 5ms 调一次，
     *   若邮箱长时间无空位（AutoRetransmission 无限重传会占死邮箱），长自旋会抢光 CPU
     *   → 所有低级任务（串口回包/屏幕/LED）被饿死，现场表现就是"板子卡死"。
     *   所以：最多空转 500 次（≈50µs）就放弃本帧；丢帧由上层看门狗（abort + 重试）处理。 */
    while (HAL_CAN_GetTxMailboxesFreeLevel(s_dev.can_instance->can_handle) == 0)
    {
        if (++guard > 500u)
        {
            s_dev.tx_dropped++; /* 记一笔：屏/摘要可看，绝不静默 */
            return;
        }
    }

    hdr.StdId = std_id;
    hdr.ExtId = 0;
    hdr.IDE = CAN_ID_STD;
    hdr.RTR = CAN_RTR_DATA;
    hdr.DLC = 8;
    hdr.TransmitGlobalTime = DISABLE;

    if (HAL_CAN_AddTxMessage(s_dev.can_instance->can_handle, &hdr, (uint8_t *)data, &mailbox) == HAL_OK)
    {
        s_dev.fb.tx_cnt++;
    }
}

/* ----------------------- 反馈帧回调（中断上下文：只拷贝，不算数） ----------------------- */
static void j8108_rx_callback(CANInstance *inst)
{
    if (inst->rx_len >= 8)
    {
        memcpy(s_dev.fb.raw, inst->rx_buff, 8);
        s_dev.fb.rx_count++;
        /* 中断上下文：用 FromISR 版本取时基（失联检测用） */
        s_dev.fb.last_rx_ms = (uint32_t)(xTaskGetTickCountFromISR() * portTICK_PERIOD_MS);
        s_dev.new_frame = 1u; /* 任务侧在临界区内取走 → 保证 8 字节来自同一帧 */
    }
}

/* --------------------------------- 初始化 --------------------------------- */
void J8108_Init(CAN_HandleTypeDef *hcan)
{
    CAN_Init_Config_s cfg = {0};

    cfg.can_handle = hcan;
    cfg.tx_id = J8108_MIT_ID; /* 主发送 ID（MIT 帧）；命令帧用裸发送 */
    cfg.rx_id = J8108_FB_ID;  /* 接收 0x781 反馈帧 */
    cfg.can_module_callback = j8108_rx_callback;
    cfg.id = &s_dev;

    s_dev.can_instance = CANRegister(&cfg);
    s_dev.init_ok = (s_dev.can_instance != NULL) ? 1u : 0u;
    s_inited = s_dev.init_ok;
    s_dev.bus_err = 0u;
    s_dev.status = (s_dev.init_ok != 0u) ? (uint8_t)J8108_ST_INIT_OK : (uint8_t)J8108_ST_INIT_FAIL;

    if (s_dev.init_ok != 0u)
    {
        LOG_I("J8108", "CAN init OK (rx_id=0x%03X registered, controller started) -> display: CAN INIT OK / WAITING",
              J8108_FB_ID);
    }
    else
    {
        LOG_E("J8108", "CAN init FAILED (CANRegister returned NULL) -> display: CAN INIT FAIL; no retry by design, check peripheral/device table");
    }
}

J8108_t *J8108_Get(void)
{
    return &s_dev;
}

/* --------------------------------- 命令帧 --------------------------------- */
void J8108_SendCmd(uint8_t cmd_code)
{
    uint8_t d[8] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};

    d[7] = cmd_code;
    j8108_tx(J8108_CMD_ID, d);
}

/* --------------------------------- MIT 帧 --------------------------------- */
/* 定点编码：raw = (v - min) / (max - min) * (2^bits - 1)，越界钳位 */
static uint16_t j8108_f2u(float v, float min, float max, uint8_t bits)
{
    float r = (v - min) / (max - min);

    if (r < 0.0f)
        r = 0.0f;
    if (r > 1.0f)
        r = 1.0f;
    return (uint16_t)(r * (float)((1u << bits) - 1u) + 0.5f);
}

void J8108_SendMIT(float p, float v, float kp, float kd, float t)
{
    /* 入参单位：**输出端** rad / rad·s⁻¹ / N·m（与反馈同口径）；正方向由 J8108_DIR 统一处理 */
    uint16_t pu = j8108_f2u(p * J8108_DIR, J8108_P_LO, J8108_P_HI, 16);
    uint16_t vu = j8108_f2u(v * J8108_DIR, J8108_V_LO, J8108_V_HI, 12);
    uint16_t kpu = j8108_f2u(kp, J8108_KP_LO, J8108_KP_HI, 12);
    uint16_t kdu = j8108_f2u(kd, J8108_KD_LO, J8108_KD_HI, 12);
    uint16_t tu = j8108_f2u(t, J8108_T_LO, J8108_T_HI, 12);
    uint8_t d[8];

    d[0] = (uint8_t)(pu >> 8);
    d[1] = (uint8_t)(pu & 0xFF);
    d[2] = (uint8_t)((vu >> 4) & 0xFF);
    d[3] = (uint8_t)(((vu & 0x0F) << 4) | ((kpu >> 8) & 0x0F));
    d[4] = (uint8_t)(kpu & 0xFF);
    d[5] = (uint8_t)((kdu >> 4) & 0xFF);
    d[6] = (uint8_t)(((kdu & 0x0F) << 4) | ((tu >> 8) & 0x0F));
    d[7] = (uint8_t)(tu & 0xFF);

    j8108_tx(J8108_MIT_ID, d);
}

/* ------------------------- 反馈解算（任务上下文调用） ------------------------- */
/* 快照（seqlock）：写侧本函数；读侧 J8108_CopySnapshot（任意任务） */
static volatile uint32_t s_snap_seq = 0u;
static J8108_Snapshot_t s_snap;

void J8108_CopySnapshot(J8108_Snapshot_t *out)
{
    uint32_t s1, s2;

    if (out == NULL)
    {
        return;
    }
    do
    {
        s1 = s_snap_seq;
        if ((s1 & 1u) != 0u)
        {
            continue; /* 正在写入：重试 */
        }
        memcpy(out, (const void *)&s_snap, sizeof(J8108_Snapshot_t));
        s2 = s_snap_seq;
    } while (s1 != s2); /* 拷贝期间被写过 → 重来 */
}

void J8108_Update(void)
{
    uint8_t d[8];
    uint16_t pos_raw, vel_raw, t_raw;
    uint32_t t = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

    /* ---- 取一帧：临界区内拷贝 8 字节（与接收中断互斥），保证同帧解算 ---- */
    if (s_dev.new_frame != 0u)
    {
        taskENTER_CRITICAL();
        memcpy(d, s_dev.fb.raw, 8u);
        s_dev.new_frame = 0u;
        taskEXIT_CRITICAL();
    }
    else
    {
        memcpy(d, (const void *)s_dev.fb.raw, 8u); /* 无新帧：沿用上一帧原始数据 */
    }

    pos_raw = (uint16_t)((d[1] << 8) | d[2]);
    vel_raw = (uint16_t)((d[3] << 4) | (d[4] >> 4));
    t_raw = (uint16_t)(((d[4] & 0x0F) << 8) | d[5]);

    /* 双编口径：POS/VEL 即输出端物理量（J8108_P_LO/HI 随 J8108_SCALE_OUTPUT_SIDE 切换），不做 ÷8 */
    s_dev.fb.err = d[0]; /* byte0 = 报错位（bit7 过载 / bit6 线圈过温 / bit5 MOS 过温 / bit4 过流 / bit3 过压 / bit2 欠压） */
    s_dev.fb.pos = J8108_P_LO + (float)pos_raw / 65535.0f * (J8108_P_HI - J8108_P_LO);
    s_dev.fb.vel = J8108_V_LO + (float)vel_raw / 4095.0f * (J8108_V_HI - J8108_V_LO);
    s_dev.fb.torque = J8108_T_LO + (float)t_raw / 4095.0f * (J8108_T_HI - J8108_T_LO);
    s_dev.fb.t_mos = (float)d[6] * 100.0f / 255.0f;
    s_dev.fb.t_rotor = (float)d[7] * 100.0f / 255.0f;

    /* ---- 链路状态刷新（屏上四态 + 日志只打"状态变化"边沿）---- */
    if (s_dev.init_ok == 0u)
    {
        s_dev.status = (uint8_t)J8108_ST_INIT_FAIL;
    }
    else if ((s_dev.bus_err = HAL_CAN_GetError(&hcan1)) != 0u)
    {
        s_dev.status = (uint8_t)J8108_ST_BUS_ERR;
    }
    else if (s_dev.fb.rx_count > 0u)
    {
        /* 200ms 内有反馈帧 = 通道活着 = 随时可发数据 */
        s_dev.status = ((t - s_dev.fb.last_rx_ms) < 200u) ? (uint8_t)J8108_ST_READY : (uint8_t)J8108_ST_INIT_OK;
    }
    else
    {
        s_dev.status = (uint8_t)J8108_ST_INIT_OK;
    }

    /* ---- 发布快照（seqlock：先奇数后偶数；读侧靠序号变化重试）---- */
    s_snap_seq++;
    memcpy(s_snap.raw, d, 8u);
    s_snap.valid = (s_dev.fb.rx_count > 0u) ? 1u : 0u;
    s_snap.status = s_dev.status;
    s_snap.init_ok = s_dev.init_ok;
    s_snap.err = s_dev.fb.err;
    s_snap.bus_err = s_dev.bus_err;
    s_snap.rx_count = s_dev.fb.rx_count;
    s_snap.tx_cnt = s_dev.fb.tx_cnt;
    s_snap.frame_age_ms = (s_dev.fb.rx_count > 0u) ? (t - s_dev.fb.last_rx_ms) : 0u;
    s_snap.pos = s_dev.fb.pos;
    s_snap.vel = s_dev.fb.vel;
    s_snap.torque = s_dev.fb.torque;
    /* 双编口径：pos/vel 已是输出端，直接换算工程单位（不除减速比） */
    s_snap.pos_deg = s_dev.fb.pos * J8108_RAD2DEG;
    s_snap.vel_dps = s_dev.fb.vel * J8108_RAD2DEG;
    s_snap.vel_rpm = s_dev.fb.vel * 9.54930f;
    s_snap.t_mos = s_dev.fb.t_mos;
    s_snap.t_rotor = s_dev.fb.t_rotor;
    s_snap.seq = s_snap_seq + 1u;
    s_snap_seq++;
}

/* ---- TX 邮箱是否"连续占用"超过 limit_ms = 无人 ACK（相位 1=正在发送，不算） ---- */
static uint8_t s_tx_busy = 0u;
static uint32_t s_tx_busy_ms = 0u;

uint8_t J8108_TxStuck(uint32_t now_ms, uint32_t limit_ms)
{
    if (!s_inited)
    {
        return 0u;
    }
    if (HAL_CAN_GetTxMailboxesFreeLevel(s_dev.can_instance->can_handle) < 3u)
    {
        if (s_tx_busy == 0u)
        {
            s_tx_busy = 1u;
            s_tx_busy_ms = now_ms;
        }
        return ((uint32_t)(now_ms - s_tx_busy_ms) >= limit_ms) ? 1u : 0u;
    }
    s_tx_busy = 0u;
    return 0u;
}

/* ---- 报错位短标签（屏/协议显示；无错返回 "OK"） ---- */
const char *J8108_ErrStr(uint8_t err)
{
    if (err == 0u)
    {
        return "OK";
    }
    if ((err & J8108_ERR_OVERLOAD) != 0u)
        return "OVERLOAD";
    if ((err & J8108_ERR_TP_COIL) != 0u)
        return "T-COIL";
    if ((err & J8108_ERR_TP_MOS) != 0u)
        return "T-MOS";
    if ((err & J8108_ERR_OC) != 0u)
        return "OVERCUR";
    if ((err & J8108_ERR_OV) != 0u)
        return "OVERVOLT";
    if ((err & J8108_ERR_UV) != 0u)
        return "UNDERVOLT";
    return "ERR?";
}

const char *J8108_StatusStr(uint8_t status)
{
    switch (status)
    {
    case J8108_ST_INIT_FAIL:
        return "INIT FAIL";
    case J8108_ST_INIT_OK:
        return "INIT OK";
    case J8108_ST_READY:
        return "READY";
    case J8108_ST_BUS_ERR:
        return "BUS ERR";
    default:
        return "?";
    }
}
