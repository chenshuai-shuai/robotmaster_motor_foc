/*
 * @Author: skybase
 * @Date: 2025-01-13 17:12:42
 * @LastEditors: skybase
 * @LastEditTime: 2025-01-17 22:00:27
 * @Description:  ᕕ(◠ڼ◠)ᕗ​
 * @FilePath: \MDK-ARM\Code\bsp_log.h
 */
#ifndef _BSP_LOG_H_
#define _BSP_LOG_H_

#include <stdio.h>
#include "string.h"
#include "main.h"

#define COLR_LOG_MODE 1

extern char _debug[1024];

#if COLR_LOG_MODE
// 显示模式枚举
typedef enum
{
    DISPLAY_MODE_DEFAULT = 0,       // 默认值
    DISPLAY_MODE_BOLD = 1,          // 高亮
    DISPLAY_MODE_UNDERLINE = 4,     // 下划线
    DISPLAY_MODE_BLINK = 5,         // 闪烁
    DISPLAY_MODE_REVERSE = 7,       // 反显
    DISPLAY_MODE_NORMAL = 22,       // 非粗体
    DISPLAY_MODE_NO_UNDERLINE = 24, // 非下划线
    DISPLAY_MODE_NO_BLINK = 25,     // 非闪烁
    DISPLAY_MODE_NO_REVERSE = 27    // 非反显
} DISPLAY_MODE;

// 前景色枚举
typedef enum
{
    FOREGROUND_COLOR_BLACK = 30,   // 黑色
    FOREGROUND_COLOR_RED = 31,     // 红色
    FOREGROUND_COLOR_GREEN = 32,   // 绿色
    FOREGROUND_COLOR_YELLOW = 33,  // 黄色
    FOREGROUND_COLOR_BLUE = 34,    // 蓝色
    FOREGROUND_COLOR_MAGENTA = 35, // 洋红
    FOREGROUND_COLOR_CYAN = 36,    // 青色
    FOREGROUND_COLOR_WHITE = 37    // 白色
} FOREGROUND_COLOR;

// 背景色枚举
typedef enum
{
    BACKGROUND_COLOR_BLACK = 40,   // 黑色
    BACKGROUND_COLOR_RED = 41,     // 红色
    BACKGROUND_COLOR_GREEN = 42,   // 绿色
    BACKGROUND_COLOR_YELLOW = 43,  // 黄色
    BACKGROUND_COLOR_BLUE = 44,    // 蓝色
    BACKGROUND_COLOR_MAGENTA = 45, // 洋红
    BACKGROUND_COLOR_CYAN = 46,    // 青色
    BACKGROUND_COLOR_WHITE = 47    // 白色
} BACKGROUND_COLOR;

extern DISPLAY_MODE dis_mode;
extern FOREGROUND_COLOR fwd_clor;
extern BACKGROUND_COLOR bak_clor;

#define LOG_PRINT(...) (sprintf(_debug, "\033[%d;%d;%dm", dis_mode, fwd_clor, bak_clor), \
                        debug_transmit((uint8_t *)_debug,                                \
                                       (strlen(_debug) + sprintf(_debug + strlen(_debug), __VA_ARGS__))))

#else
#define LOG_PRINT(...) (debug_transmit((uint8_t *)_debug, sprintf(_debug, __VA_ARGS__)))

#endif

/**
 * @brief 日志功能的原始函数,需要用户自己自定义
 *
 */
void debug_transmit(uint8_t *data, uint16_t len);


/**
 * 级别日志输出,建议使用这些宏来输出日志
 * @note 如果不使用级别日志输出使用默认日志接口 LOG_PRINT(...) 会使用默认的基本颜色
 */
#if COLR_LOG_MODE
// information level
#define LOGINFO(...) (dis_mode = DISPLAY_MODE_DEFAULT, fwd_clor = FOREGROUND_COLOR_BLACK, bak_clor = BACKGROUND_COLOR_WHITE, LOG_PRINT("I:" __VA_ARGS__))

// warning level
#define LOGWARNING(...) (dis_mode = DISPLAY_MODE_DEFAULT, fwd_clor = FOREGROUND_COLOR_BLACK, bak_clor = BACKGROUND_COLOR_YELLOW, LOG_PRINT("W:"__VA_ARGS__))

// error level
#define LOGERROR(...) (dis_mode = DISPLAY_MODE_DEFAULT, fwd_clor = FOREGROUND_COLOR_BLACK, bak_clor = BACKGROUND_COLOR_RED, LOG_PRINT("E:"__VA_ARGS__))

#else

#define LOGINFO(...) LOG_PRINT("I:" __VA_ARGS__)
#define LOGWARNING(...) (LOG_PRINT("W:"__VA_ARGS__))
#define LOGERROR(...) (LOG_PRINT("E:"__VA_ARGS__))


#endif

/*用户自定义LOG区域,可以在理解该库的基础上再此区域内添加日志打印*/

#endif
