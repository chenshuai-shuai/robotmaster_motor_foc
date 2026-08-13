/*
 * @Author: skybase
 * @Date: 2025-01-14 09:45:46
 * @LastEditors: skybase
 * @LastEditTime: 2025-03-30 13:41:12
 * @Description:  ?(???)??
 * @FilePath: \mcu_bsp\button\button.c
 */
#include "button.h"

static int idx;
static ButtonInstance *button_instance[BUTTON_NUM] = {0};

/**
 * @brief 使用HAL库对按键信息进行更新
 *
 * @note 根据不同单片机用户自己定义
 */
static void buttons_Check(int id)
{

    button_instance[id]->key_state = !!(xbox_data_frame.buttons & button_instance[id]->xbox_button_enum);
}

void button_Init(char *name,
                 uint16_t xbox_button_enum,
                 TRIGGER_LEVEL trigger_level,
                 button_module_callback button_down_callback,
                 button_module_callback button_continue_callback,
                 button_module_callback button_up_callback)
{
    ButtonInstance *_instance = malloc(sizeof(ButtonInstance));
    button_instance[idx++] = _instance;
    _instance->id = idx;
    _instance->xbox_button_enum = xbox_button_enum;
    _instance->name = name;
    _instance->trigger_level = trigger_level;
    _instance->button_down_callback = button_down_callback;
    _instance->button_continue_callback = button_continue_callback;
    _instance->button_up_callback = button_up_callback;
}

static void button_Callback(int i)
{
    if (button_instance[i]->button_down_callback != NULL && button_instance[i]->press_state == BT_Down)
    {
        button_instance[i]->button_down_callback(button_instance[i]->name);
    }
    else if (button_instance[i]->button_continue_callback != NULL && button_instance[i]->press_state == BT_Pressing)
    {
        button_instance[i]->button_continue_callback(button_instance[i]->name);
    }
    else if (button_instance[i]->button_up_callback != NULL && button_instance[i]->press_state == BT_Up)
    {
        button_instance[i]->button_up_callback(button_instance[i]->name);
    }
}
/**
 * @brief buttons的状态更新函数
 *
 * @param time_base 状态时间时基（ms)
 */
void button_State_Update(int time_base)
{
    ButtonInstance *_instance;

    static int dead_pluse[BUTTON_NUM] = {0};
    static int cof[BUTTON_NUM] = {0};
    static int button_pressed_cal[BUTTON_NUM] = {0};

    for (int i = 0; i < idx; i++)
    {
        buttons_Check(i);
        button_Callback(i);
        _instance = button_instance[i];

        _instance->key_state = _instance->trigger_level == TRIGGER_LEVEL_HIGH ? _instance->key_state : (!_instance->key_state);

        _instance->press_state = BT_NONE;
        if (_instance->key_state && button_pressed_cal[i] == 0 && --dead_pluse[i] <= 0)
        {
            if (cof[i] == 1)
            {
                _instance->press_state = BT_Down;
                button_pressed_cal[i]++;
                continue;
            }
            else
            {
                cof[i] = 1;
                continue;
            }
        }
        else if (button_pressed_cal[i] > 0)
        {
            _instance->press_state = BT_Pressing;
            button_pressed_cal[i]++;
            _instance->press_time = button_pressed_cal[i] * 1.0 * time_base / 1000;
        }
        if (!_instance->key_state && button_pressed_cal[i] != 0)
        {
            _instance->press_state = BT_Up;
            button_pressed_cal[i] = 0;
            dead_pluse[i] = 3;
            cof[i] = 0;
            continue;
        }
    }
}
