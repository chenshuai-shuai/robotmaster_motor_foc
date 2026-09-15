/*
 * bsp_key.c - 板载用户按键（PB2）实现：GPIO + 10ms 扫描 + 事件分发 + 日志
 *
 * 日志约定：全部英文/ASCII（串口终端下中文乱码）。
 * 开发期日志（FEATURE_VERBOSE_LOG=1）：
 *   - 原始电平每次跳变都打（用于确认 PB2 有效电平方向 / 接触是否可靠）
 *   - 长按过程中每 500ms 打一次累计时长（观察 2s 阈值进度）
 */
#include "bsp_key.h"

#include "main.h"
#include "task.h"
#include "bsp_log.h"
#include "string.h"
#include "feature_config.h"

static KeyCore_t s_core;
static EventGroupHandle_t s_evt_group = NULL;
static TaskHandle_t s_key_task = NULL;
static volatile uint8_t s_raw_level = 1u;
static volatile uint32_t s_hold_ms = 0u;
static KeyCore_Msg_t s_last_msg; /* 最近一次事件快照（提示行/日志用；事件位仍由各消费者自行等待） */
static volatile uint32_t s_last_msg_ms = 0u;

/* ------------------------------- 初始化 ------------------------------- */
void Key_Init(void)
{
    GPIO_InitTypeDef gpio = {0};

    if (s_evt_group == NULL)
    {
        s_evt_group = xEventGroupCreate();
        if (s_evt_group == NULL)
        {
            LOG_E("Key", "xEventGroupCreate FAILED (heap?). Key events will not be delivered.");
        }
    }

    if (s_core.tick_ms != 0u)
    {
        return; /* 幂等：已初始化 */
    }

    __HAL_RCC_GPIOB_CLK_ENABLE(); /* CubeMX 未配置 PB2，此处自备时钟 */
    gpio.Pin = KEY_GPIO_PIN;
    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pull = KEY_GPIO_PULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(KEY_GPIO_PORT, &gpio);

    KeyCore_Init(&s_core, KEY_ACTIVE_LEVEL, KEY_SCAN_TICK_MS);

    s_raw_level = (HAL_GPIO_ReadPin(KEY_GPIO_PORT, KEY_GPIO_PIN) == GPIO_PIN_SET) ? 1u : 0u;

    LOG_I("Key", "init: PB2 input (internal pull-up) | active_level=%u (0=low when pressed) | idle raw level=%u (expect %u)",
          KEY_ACTIVE_LEVEL, s_raw_level, (KEY_ACTIVE_LEVEL == 0u) ? 1u : 0u);
    LOG_I("Key", "timing: scan=%ums debounce=%ums short-press=%u..%ums multi-gap=%u..%ums stuck=%ums",
          KEY_SCAN_TICK_MS, KEYC_DEBOUNCE_MS, KEYC_CLICK_MIN_MS, KEYC_CLICK_MAX_MS,
          KEYC_GAP_MIN_MS, KEYC_GAP_MAX_MS, KEYC_STUCK_MS);

    if (s_raw_level == KEY_ACTIVE_LEVEL)
    {
        LOG_W("Key", "idle level equals active level -> key pressed at boot OR level polarity is inverted; check KEY_ACTIVE_LEVEL");
    }
}

/* ----------------------------- 单次扫描 ----------------------------- */
static const char *key_evt_name(KeyCore_Event_e e)
{
    switch (e)
    {
    case KEYC_EVT_CLICK:
        return "CLICK";
    case KEYC_EVT_DOUBLE:
        return "DOUBLE";
    case KEYC_EVT_HOLD_RELEASE:
        return "HOLD";
    case KEYC_EVT_STUCK:
        return "STUCK";
    default:
        return "NONE";
    }
}

static void key_scan_once(void)
{
    KeyCore_Msg_t msg;
    EventBits_t bit = 0u;

    s_raw_level = (HAL_GPIO_ReadPin(KEY_GPIO_PORT, KEY_GPIO_PIN) == GPIO_PIN_SET) ? 1u : 0u;
    s_hold_ms = s_core.hold_ms;

    /* 日志策略（用户要求）：只打"事件"，高频基础检测（每次电平跳变 / 每 500ms 长按进度）不再打印 */
    if (KeyCore_Step(&s_core, s_raw_level, &msg) == 0u)
    {
        return;
    }
    s_hold_ms = s_core.hold_ms;

    switch (msg.evt)
    {
    case KEYC_EVT_CLICK:
        bit = KEY_BIT_CLICK;
        LOG_I("Key", "CLICK short-press=%ums gap=%ums clicks=%u | total click=%lu double=%lu hold=%lu ignored=%lu",
              (unsigned)msg.hold_ms, (unsigned)msg.gap_ms, msg.clicks,
              (unsigned long)s_core.cnt_click, (unsigned long)s_core.cnt_double,
              (unsigned long)s_core.cnt_hold, (unsigned long)s_core.cnt_ignored);
        break;
    case KEYC_EVT_DOUBLE:
        bit = KEY_BIT_DOUBLE;
        LOG_I("Key", "DOUBLE last-press=%ums gap=%ums clicks=%u", (unsigned)msg.hold_ms,
              (unsigned)msg.gap_ms, msg.clicks);
        break;
    case KEYC_EVT_HOLD_RELEASE:
        bit = KEY_BIT_HOLD_RELEASE;
        LOG_I("Key", "HOLD released after %ums (%.2fs) -> action layer decides by per-row threshold (enable row = 2000ms)",
              (unsigned)msg.hold_ms, (double)msg.hold_ms / 1000.0);
        break;
    case KEYC_EVT_STUCK:
        bit = KEY_BIT_STUCK;
        LOG_E("Key", "STUCK: level active for %ums -> key ignored (press/wiring problem); check PB2 hardware",
              (unsigned)msg.hold_ms);
        break;
    default:
        break;
    }

    if ((bit != 0u) && (s_evt_group != NULL))
    {
        xEventGroupSetBits(s_evt_group, bit);
    }

    s_last_msg = msg;
    s_last_msg_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

/* ------------------------------ 扫描任务 ------------------------------ */
static void key_task(void *arg)
{
    (void)arg;
    Key_Init();
    LOG_I("Key", "task running (prio %u, stack %u words)", KEY_TASK_PRIORITY, KEY_TASK_STACK_WORDS);

    for (;;)
    {
        key_scan_once();
        /* 用 vTaskDelay（不用 vTaskDelayUntil）：CubeMX 生成的 FreeRTOSConfig.h 中
           INCLUDE_vTaskDelayUntil=0，不引入该符号以免改动生成区配置；
           本任务只需"≈10ms 节拍"（事件机的时基即扫描周期），±5% 抖动对
           2s 长按阈值 / 300ms 连击窗口判定无实质影响。 */
        vTaskDelay(pdMS_TO_TICKS(KEY_SCAN_TICK_MS));
    }
}

/* ------------------------------ 对外接口 ------------------------------ */
void Key_Task_Init(void)
{
    BaseType_t ok;

    Key_Init(); /* 先把事件组建好：消费者任务可能先于本任务运行 */
    ok = xTaskCreate(key_task, "KeyTask", KEY_TASK_STACK_WORDS, NULL, KEY_TASK_PRIORITY, &s_key_task);
    if (ok != pdPASS)
    {
        LOG_E("Key", "xTaskCreate FAILED (heap/stack?) -> KEY DISABLED");
    }
    else
    {
        LOG_I("Key", "task created (KeyTask)");
    }
}

EventGroupHandle_t Key_EventGroup(void)
{
    return s_evt_group;
}

uint8_t Key_IsPressed(void)
{
    return s_core.stable_pressed;
}

uint32_t Key_GetHoldMs(void)
{
    return s_hold_ms;
}

uint8_t Key_GetRawLevel(void)
{
    return s_raw_level;
}

void Key_GetCore(KeyCore_t *out)
{
    if (out != NULL)
    {
        memcpy(out, &s_core, sizeof(KeyCore_t));
    }
}

void Key_GetLastMsg(KeyCore_Msg_t *out, uint32_t *ms)
{
    if (out != NULL)
    {
        *out = s_last_msg;
    }
    if (ms != NULL)
    {
        *ms = s_last_msg_ms;
    }
}
