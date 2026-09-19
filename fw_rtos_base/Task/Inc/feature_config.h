/*
 * feature_config.h - 工程配置唯一入口：预设组合(profile) + 功能宏 + 依赖自检
 *
 * 用法：
 *   1) 选一个组合：改下面 §1 的 CFG_PROFILE，或编译期覆盖 -DCFG_PROFILE=PROFILE_XXX
 *      （覆盖方式给 profile 编译矩阵用，见 tools/check_profiles.py）
 *   2) "插了哪块屏"这类硬件选择在 §4，不随 profile 变
 *   3) 加新功能：在 §2 每个 profile 里加一行 + 在 §6 写清依赖规则
 *
 * 规矩（全文见 docs/规范_功能宏与模块化.md）：
 *   R1 宏只做"裁剪"，不改行为（行为差异走参数/命令，不许藏在宏里）
 *   R2 依赖必须显式 + 编译期可查（禁止"反正能链接就算对"）
 *   R3 未启用模块的实现文件编译成空对象（M3 起逐模块落地）
 *   R7 安全模块（J8108）被关闭时必须告警可见，不许静默消失
 *
 * 注意：所有 LOG_* 文本必须英文/ASCII（串口终端下中文会乱码）。
 */
#ifndef FEATURE_CONFIG_H
#define FEATURE_CONFIG_H

/* ==================== 1. 预设组合（选一个） ==================== */

#define PROFILE_FULL      1u /* 整机：电机+屏+串口+按键+LED（= 2026-09-19 交付状态） */
#define PROFILE_DISP_DEV  2u /* 屏/UI 分块调试：有屏无电机（电机不抢时基，排显示干扰） */
#define PROFILE_MOTOR_DEV 3u /* 电机分块调试：有电机无屏（保时优先，排屏刷干扰） */
#define PROFILE_MINIMAL   4u /* 板级 bring-up：只有串口日志 + LED */

#ifndef CFG_PROFILE
/* ★★ 当前默认组合（编译下载的就是这个）★★
 * 彩屏阶段 = PROFILE_DISP_DEV（有屏无电机）：电机模块整体不参与编译，不抢时基。
 * 彩屏整合完成后改回 PROFILE_FULL 即交付版。开机会在 @BOOT 横幅自报组合名。 */
#define CFG_PROFILE PROFILE_DISP_DEV
#endif

/* ==================== 2. 功能宏（由 profile 展开；勿手工改） ==================== */

#if CFG_PROFILE == PROFILE_FULL
#define BOOT_PROFILE_NAME    "FULL"
#define FEATURE_J8108        1u
#define FEATURE_KEY          1u
#define FEATURE_DISP_UI      1u
#define FEATURE_MONITOR_TASK 1u
#define FEATURE_SERIAL_CTRL  1u
#define FEATURE_LED_TASK     1u
#elif CFG_PROFILE == PROFILE_DISP_DEV
#define BOOT_PROFILE_NAME    "DISP_DEV"
#define FEATURE_J8108        0u
#define FEATURE_KEY          1u
#define FEATURE_DISP_UI      1u
#define FEATURE_MONITOR_TASK 1u
#define FEATURE_SERIAL_CTRL  1u
#define FEATURE_LED_TASK     1u
#elif CFG_PROFILE == PROFILE_MOTOR_DEV
#define BOOT_PROFILE_NAME    "MOTOR_DEV"
#define FEATURE_J8108        1u
#define FEATURE_KEY          0u
#define FEATURE_DISP_UI      0u
#define FEATURE_MONITOR_TASK 0u
#define FEATURE_SERIAL_CTRL  1u
#define FEATURE_LED_TASK     1u
#elif CFG_PROFILE == PROFILE_MINIMAL
#define BOOT_PROFILE_NAME    "MINIMAL"
#define FEATURE_J8108        0u
#define FEATURE_KEY          0u
#define FEATURE_DISP_UI      0u
#define FEATURE_MONITOR_TASK 0u
#define FEATURE_SERIAL_CTRL  0u
#define FEATURE_LED_TASK     1u
#else
#error "unknown CFG_PROFILE (use PROFILE_FULL / PROFILE_DISP_DEV / PROFILE_MOTOR_DEV / PROFILE_MINIMAL)"
#endif

/* ==================== 3. 重型功能（默认关；它们自带一整套逻辑，按需单独开） ==================== */

#define FEATURE_SD_CARD   0u /* SD 卡任务 + FatFs（开=同时会有 [sdhal]/[sdfs] 日志） */
#define FEATURE_SD_CLI    0u /* SD 卡串口命令行（STM32> 提示符） */
#define FEATURE_CAR_TASKS 0u /* 小车：舵机 + 遥控 + main_cpp() */

/* ==================== 4. 显示驱动选择（硬件相关，不随 profile 变） ==================== */
/* 两块屏共用 A 板 OLED 口（7P）的同一组引脚 → 同一时刻只能开一个。
 * ★ 这两个宏允许命令行 -D 覆盖（道理同 CFG_PROFILE）：屏是**硬件选择**，不属于任何 profile，
 *   组合矩阵默认永远编不到"没被选中那个驱动"的函数体 → tools/check_profiles.py 专门用
 *   -DFEATURE_DISP_ST7735S_SPI=1 -DFEATURE_DISP_SH1106_I2C=0 过一遍彩屏驱动（S2 烧板前必跑）。 */

#ifndef FEATURE_DISP_SH1106_I2C
#define FEATURE_DISP_SH1106_I2C  0u /* 备用：1.3" SH1106 软 I2C（PB10=SCL / PB9=SDA） */
#endif
#ifndef FEATURE_DISP_ST7735S_SPI
#define FEATURE_DISP_ST7735S_SPI 1u /* ★ 现役（2026-09-19 起）：1.8" ST7735S 彩屏 128x160（PB3=SCK PA7=MOSI PB9=RS PB10=CS PA6=SDO） */
#endif

#if FEATURE_DISP_ST7735S_SPI
#define DISP_DRIVER_NAME "ST7735S-SPI"
#elif FEATURE_DISP_SH1106_I2C
#define DISP_DRIVER_NAME "SH1106-I2C"
#else
#define DISP_DRIVER_NAME "none"
#endif

/* 派生开关：内置 ASCII 点阵（mcu_bsp/oled/OLED_Data.c）被**两个驱动共用** —— 任一驱动在就有用。
 * 少了它，选中彩屏时 OLED_Data.c 会被 SH1106 的宏编成空对象 → 驱动 B 链接期找不到字模。 */
#define FEATURE_DISP_FONTS (((FEATURE_DISP_SH1106_I2C) != 0u) || ((FEATURE_DISP_ST7735S_SPI) != 0u))

/* ==================== 5. 日志详略 ==================== */

#define FEATURE_VERBOSE_LOG 1u /* 1=开发期详日志（只打"有用且低频"的：CAN 前 10 帧原始字节+解算值）；0=关 */

/* ==================== 6. 依赖矩阵（编译期自检；规则见 docs/规范_功能宏与模块化.md §5） ==================== */

#if FEATURE_DISP_UI && ((FEATURE_DISP_SH1106_I2C + FEATURE_DISP_ST7735S_SPI) != 1)
#error "FEATURE_DISP_UI needs exactly ONE display driver (see section 4)"
#endif
#if (FEATURE_DISP_SH1106_I2C + FEATURE_DISP_ST7735S_SPI) > 1
#error "two display drivers enabled: they share the same OLED-port pins"
#endif
#if FEATURE_MONITOR_TASK && !FEATURE_DISP_UI
#error "FEATURE_MONITOR_TASK requires FEATURE_DISP_UI (it renders the 4 pages to the screen)"
#endif
#if FEATURE_MONITOR_TASK && !FEATURE_KEY
#error "FEATURE_MONITOR_TASK requires FEATURE_KEY (4 页界面靠板载按键翻页，没有按键就没有 UI 输入)"
#endif
/* ★ 临时规则已于 2026-09-19 删除（S1/S2 落地）：disp_port.h 契约 + 两个驱动
 *   (disp_sh1106_i2c.c / disp_st7735s_spi.c) 都在 mcu_bsp/disp/，选中哪个宏就编哪个。 */
#if FEATURE_SD_CLI && !FEATURE_SD_CARD
#error "FEATURE_SD_CLI requires FEATURE_SD_CARD"
#endif

#endif /* FEATURE_CONFIG_H */
