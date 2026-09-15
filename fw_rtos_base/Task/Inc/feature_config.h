/*
 * feature_config.h - 功能开关（开发阶段用宏裁剪无关功能）
 *
 * 用法：只改这里的 0/1，重新编译即可。默认 = 只保留「8108 电机 + 按键 + OLED + 日志」，
 *       关掉小车（舵机/遥控）、SD 卡、串口命令行 —— 既省 heap（32KB 很紧张，
 *       任务创建失败会静默不跑）也消除 [sdhal]/[sdfs] 的 D: 日志刷屏。
 *
 * 注意：所有 LOG_* 文本必须用**英文/ASCII**（串口终端下中文会乱码）。
 */
#ifndef FEATURE_CONFIG_H
#define FEATURE_CONFIG_H

/* ---------------- 当前开发的功能（开） ---------------- */
#define FEATURE_J8108 1u     /* 8108 关节电机任务（CAN1 监听 / 后续控制） */
#define FEATURE_KEY 1u       /* 板载按键 PB2（事件机：单击/双击/长按/卡死） */
#define FEATURE_OLED_UI 1u   /* OLED 单页 8 行监护界面 */
#define FEATURE_LED_TASK 1u  /* LED 流水灯（保留：一眼看出系统在跑） */

/* ---------------- 与电机控制无关（关） ---------------- */
#define FEATURE_SD_CARD 0u  /* SD 卡任务 + FatFs（关掉同时消除 sdhal/sdfs 的 D: 刷屏日志） */
#define FEATURE_SD_CLI 0u   /* 串口命令行（关掉后 uart10 纯日志输出，无 STM32> 提示符） */
#define FEATURE_CAR_TASKS 0u /* 小车：舵机 + 遥控 + main_cpp() */

/* ---------------- 日志详略 ---------------- */
#define FEATURE_VERBOSE_LOG 1u /* 1=开发期详日志（仅保留"有用且低频"的：CAN 前 10 帧原始字节+解算值）；0=关。
                                * 注意：按键逐次电平跳变 / 长按每 500ms 进度 / UI 每 2s 摘要等**高频噪声已移除**（用户要求"只打事件"）。 */

#endif /* FEATURE_CONFIG_H */
