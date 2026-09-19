/*
 * board_config.h - **A 板（RoboMaster 开发板 A 型 / STM32F427IIH6）板级常量唯一出处**
 *
 * 规范（docs/规范_功能宏与模块化.md K5）：
 *   · 引脚 / 外设 / 尺寸常量集中在本文件；模块实现里**不再出现裸值**（GPIOB / GPIO_PIN_2 …）
 *   · 任务优先级 / 周期 / 栈大小**不在这里** —— 那是功能参数，留在各模块自己的头里
 *   · 换板子/换屏只改本文件（模块代码零改动）
 *
 * 验证：`tools/verify_dev.py` 断言 Task/ 与 mcu_bsp/ 下的裸引脚值出现次数为 0。
 *
 * 允许例外（已登记，M7 一并处置）：
 *   · mcu_bsp/Motor/dc_motor.c —— 不在工程编译集内（uvprojx 无此文件）
 *   · mcu_bsp/sd/sd_sdio.h     —— SD 功能当前全 profile 关闭（sd=0），SDIO 引脚归 sd 模块内部
 */
#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H

#include "main.h" /* GPIO_TypeDef / GPIO_PIN_x（CubeMX 生成） */

/* ================================ 用户 LED ================================
 * A 板：红 = PE11，绿 = PF14，另有外接 8 灯排（PG1..PG8，低电平点亮） */
#define BRD_LED_RED_PORT    GPIOE
#define BRD_LED_RED_PIN     GPIO_PIN_11
#define BRD_LED_GRN_PORT    GPIOF
#define BRD_LED_GRN_PIN     GPIO_PIN_14
#define BRD_LED_EXT_PORT    GPIOG
#define BRD_LED_EXT_MASK    (GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_3 | GPIO_PIN_4 | \
                             GPIO_PIN_5 | GPIO_PIN_6 | GPIO_PIN_7 | GPIO_PIN_8)
#define BRD_LED_ON_LEVEL    GPIO_PIN_RESET /* 低电平点亮 */
#define BRD_LED_EXT_FIRST_PIN GPIO_PIN_1   /* 外接灯排第 0 个灯的引脚位（后续用 <<i 展开） */

/* ================================ 用户按键 ================================
 * A 板 KEY = PB2，**高电平有效**（2026-09-15 实测，内部上拉兜底） */
#define BRD_KEY_PORT        GPIOB
#define BRD_KEY_PIN         GPIO_PIN_2
#define BRD_KEY_ACTIVE_HIGH 1

/* ========================= 屏幕口（A 板 7P OLED 口，1.0mm）=========================
 * 口位定义（丝印 pin1 → pin7）：
 *   pin1 = PA6（现用 MISO/字库 SDO · 老用法 BUTTON_AD）
 *   pin2 = PA7（SPI1_MOSI / SDI）
 *   pin3 = PB3（SPI1_SCK / SCL）
 *   pin4 = PB9（DC/RS · 老用法软 I2C SDA）
 *   pin5 = PB10（CS · 老用法软 I2C SCL）
 *   pin6 = GND      pin7 = 3V3
 * 换屏 = 改这里的引用（模块代码不动）。 */

/* 现役：SH1106 软 I2C（SCL=PB10 / SDA=PB9） */
#define BRD_SCR_I2C_SCL_PORT GPIOB
#define BRD_SCR_I2C_SCL_PIN  GPIO_PIN_10 /* 口 pin5 */
#define BRD_SCR_I2C_SDA_PORT GPIOB
#define BRD_SCR_I2C_SDA_PIN  GPIO_PIN_9  /* 口 pin4 */

/* 彩屏（ST7735S）：SPI1 + CS/RS/MISO（S1 落地时启用） */
#define BRD_SCR_SPI_SCK_PORT  GPIOB
#define BRD_SCR_SPI_SCK_PIN   GPIO_PIN_3 /* 口 pin3 */
#define BRD_SCR_SPI_MOSI_PORT GPIOA
#define BRD_SCR_SPI_MOSI_PIN  GPIO_PIN_7 /* 口 pin2 */
#define BRD_SCR_SPI_MISO_PORT GPIOA
#define BRD_SCR_SPI_MISO_PIN  GPIO_PIN_6 /* 口 pin1（字库 SDO，读汉字用） */
#define BRD_SCR_RS_PORT       GPIOB
#define BRD_SCR_RS_PIN        GPIO_PIN_9 /* 口 pin4（DC） */
#define BRD_SCR_CS_PORT       GPIOB
#define BRD_SCR_CS_PIN        GPIO_PIN_10 /* 口 pin5（**低=LCD / 高=字库**，双从机反相） */

#endif /* BOARD_CONFIG_H */
