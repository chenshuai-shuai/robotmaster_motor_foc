/*
 * @Author: skybase
 * @Date: 2025-01-14 09:45:50
 * @LastEditors: skybase
 * @LastEditTime: 2025-03-30 13:41:37
 * @Description:  ?(???)??
 * @FilePath: \mcu_bsp\button\button.h
 */
#ifndef BUTTON_H_
#define BUTTON_H_

#include "stdio.h"
#include "malloc.h"
#include "stdint.h"
#include "string.h"


#define BUTTON_NUM 14

typedef enum
{
    BT_NONE = 0,
    BT_Down,
    BT_Pressing,
    BT_Up,
} PREES_STATE;

typedef enum
{
    TRIGGER_LEVEL_HIGH = 0,
    TRIGGER_LEVEL_LOW,
} TRIGGER_LEVEL;

typedef void (*button_module_callback)(void*);

typedef struct
{
    int id;
    char* name;

    float press_time; //(s)

    uint8_t key_state; // 存储按键检测的值

    // 当前按键状态
    PREES_STATE press_state;
    // 触发电平
    TRIGGER_LEVEL trigger_level;

    // 对应三个按键触发条件
    button_module_callback button_down_callback;
    button_module_callback button_continue_callback;
    button_module_callback button_up_callback;

    // // HAl库按键接口
    // GPIO_TypeDef *GpioPort;
    // uint32_t GpioPin;
    uint16_t xbox_button_enum;

} ButtonInstance;

void button_Init(char *name,
                 uint16_t xbox_button_enum,
                 TRIGGER_LEVEL trigger_level,
                 button_module_callback button_down_callback,
                 button_module_callback button_continue_callback,
                 button_module_callback button_up_callback);

void button_State_Update(int time_base);

#endif
