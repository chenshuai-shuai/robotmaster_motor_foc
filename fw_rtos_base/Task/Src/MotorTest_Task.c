/*
 * MotorTest_Task.c - 单电机测试任务（M2006 + C610）
 *
 * 目标：先跑通 1 台电机（开环电流斜坡），验证 CAN 通信 + 反馈解析，
 *       为四轮麦轮小车（4×M2006+C610）打基础。
 *
 * 流程（自动状态机）：
 *   IDLE(2s) → RAMP_UP(电流 0→0.8A, 5s) → HOLD(0.5A, 3s) → RAMP_DOWN(→0, 3s) → 停 → 循环
 * 1kHz 控制循环更新电流；1Hz 串口打印反馈（角度/转速/电流原始值）。
 *
 * 用法：在 main_cpp() 或任务入口调用 MotorTest_Task_Init();
 * 日志串口：uart10 实例（见 mcu_bsp/uart/uart10_def.c），CubeMX 需启用对应 USART。
 */
#include "MotorTest_Task.h"

#include "FreeRTOS.h"
#include "task.h"
#include "c610.h"
#include "bsp_log.h"
#include "main.h"

/* ---- C610 电流换算（量程 -10000~10000 ↔ ±10A） ---- */
#define MOTOR_CURRENT_MAX_A 10.0f
#define MOTOR_CURRENT_RAW_MAX 10000

/* ---- 测试参数 ---- */
#define RAMP_UP_CURRENT_A 0.8f   /* 开环斜坡峰值电流（A），先小后大 */
#define HOLD_CURRENT_A 0.5f
#define RAMP_UP_MS 5000
#define HOLD_MS 3000
#define RAMP_DOWN_MS 3000
#define IDLE_MS 2000

typedef enum
{
    TEST_IDLE = 0,
    TEST_RAMP_UP,
    TEST_HOLD,
    TEST_RAMP_DOWN,
    TEST_DONE
} TestPhase_e;

static TaskHandle_t s_task_handle;
static TestPhase_e s_phase = TEST_IDLE;
static uint32_t s_phase_start_ms = 0;

static int16_t current_A_to_raw(float amp)
{
    int32_t raw = (int32_t)(amp / MOTOR_CURRENT_MAX_A * MOTOR_CURRENT_RAW_MAX);
    if (raw > MOTOR_CURRENT_RAW_MAX) raw = MOTOR_CURRENT_RAW_MAX;
    if (raw < -MOTOR_CURRENT_RAW_MAX) raw = -MOTOR_CURRENT_RAW_MAX;
    return (int16_t)raw;
}

/* 获取系统运行毫秒（FreeRTOS tick 换算） */
static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static void phase_enter(TestPhase_e phase)
{
    s_phase = phase;
    s_phase_start_ms = now_ms();
}

static void motor_test_task(void *arg)
{
    (void)arg;
    uint32_t last_ctrl_ms = 0;
    uint32_t last_print_ms = 0;

    /* 注册 1 组 C610（组0，命令帧 0x200 → 控制 ID 1~4；这里只用 1 号电机） */
    C610_Group_Register(&hcan1, 0, 0x200);

    /* 使能 1 号电机（其余 3 个电机保持禁用，电流自动清零） */
    C610_Group_instnce[0]->motor_instnce[0].motor_cofig.motor_enable_flag = MOTOR_ENABLED;

    LOG_I("MotorTest", "C610/M2006 单电机测试启动");
    LOG_I("MotorTest", "CAN1 1Mbps, 命令帧 0x200, 反馈 0x201, 电流量程 ±10A");
    phase_enter(TEST_IDLE);

    while (1)
    {
        uint32_t t = now_ms();

        /* ===== 1kHz 控制循环 ===== */
        if (t - last_ctrl_ms >= 1)
        {
            last_ctrl_ms = t;
            float cur = 0.0f;
            uint32_t elapsed = t - s_phase_start_ms;

            switch (s_phase)
            {
            case TEST_IDLE:
                cur = 0.0f;
                if (elapsed >= IDLE_MS)
                {
                    phase_enter(TEST_RAMP_UP);
                    LOG_I("MotorTest", "→ RAMP_UP (0→%.1fA, %dms)", RAMP_UP_CURRENT_A, RAMP_UP_MS);
                }
                break;
            case TEST_RAMP_UP:
                cur = RAMP_UP_CURRENT_A * (float)elapsed / (float)RAMP_UP_MS;
                if (elapsed >= RAMP_UP_MS)
                {
                    phase_enter(TEST_HOLD);
                    LOG_I("MotorTest", "→ HOLD (%.1fA)", HOLD_CURRENT_A);
                }
                break;
            case TEST_HOLD:
                cur = HOLD_CURRENT_A;
                if (elapsed >= HOLD_MS)
                {
                    phase_enter(TEST_RAMP_DOWN);
                    LOG_I("MotorTest", "→ RAMP_DOWN");
                }
                break;
            case TEST_RAMP_DOWN:
                cur = HOLD_CURRENT_A * (1.0f - (float)elapsed / (float)RAMP_DOWN_MS);
                if (elapsed >= RAMP_DOWN_MS)
                {
                    phase_enter(TEST_DONE);
                    LOG_I("MotorTest", "→ DONE (电机停)");
                }
                break;
            case TEST_DONE:
                cur = 0.0f;
                if (elapsed >= IDLE_MS * 2)
                {
                    phase_enter(TEST_IDLE);
                    LOG_I("MotorTest", "→ IDLE (下一轮)");
                }
                break;
            default:
                break;
            }

            /* 下发电流命令（未使能电机由驱动自动清零） */
            C610_Group_instnce[0]->motor_instnce[0].set.current = current_A_to_raw(cur);
            C610_Group_Set_Current(0);
        }

        /* ===== 1Hz 反馈打印 ===== */
        if (t - last_print_ms >= 1000)
        {
            last_print_ms = t;
            Motor_Controller_struct *m = &C610_Group_instnce[0]->motor_instnce[0];
            /* 注意：get 里是 CAN 原始值（驱动未换算） */
            int16_t ecd_raw = (int16_t)m->get.deg_pos;      /* 0~8191 转子机械角度 */
            int16_t rpm_raw = (int16_t)m->get.velocity;     /* 单位 RPM */
            int16_t cur_raw = (int16_t)m->get.current;      /* 实际输出转矩原始值 */

            LOG_I("MotorTest", "phase=%d | 角度=%d(%.1f°) 转速=%dRPM 电流raw=%d | 命令%.2fA",
                   (int)s_phase, ecd_raw, (float)ecd_raw / 8191.0f * 360.0f,
                   rpm_raw, cur_raw, C610_RAW_TO_CURRENT(m->set.current));
        }

        vTaskDelay(1);
    }
}

void MotorTest_Task_Init(void)
{
    xTaskCreate(motor_test_task, "MotorTest", 512, NULL, 5, &s_task_handle);
}
