/**
 * @file    sd_sdio.h
 * @brief   SD 卡 SDIO 底层驱动接口（四轮麦轮小车 A 板）
 *
 * 硬件（对照 A 板原理图）：
 *   SDIO_D0=PC8  SDIO_D1=PC9  SDIO_D2=PC10  SDIO_D3=PC11
 *   SDIO_CK =PC12  SDIO_CMD=PD2  卡检测 SD_EXTI=PE15（机械开关）
 *   MicroSD 座 J6（47352-1001），全 ESD 保护，VDD 常供电 3.3V
 * 时钟：SDIOCLK = 48MHz（PLLQ），SDIO_CK = 24MHz（ClockDiv=0）
 */
#ifndef __SD_SDIO_H
#define __SD_SDIO_H

#include "main.h"
#include "stm32f4xx_hal.h"

/* ---- 硬件参数（集中配置，改板必须对照原理图） ---- */
#define SD_GPIO_PORT         GPIOC
#define SD_D0_PIN            GPIO_PIN_8
#define SD_D1_PIN            GPIO_PIN_9
#define SD_D2_PIN            GPIO_PIN_10
#define SD_D3_PIN            GPIO_PIN_11
#define SD_CK_PIN            GPIO_PIN_12
#define SD_CMD_PORT          GPIOD
#define SD_CMD_PIN           GPIO_PIN_2
#define SD_DETECT_PORT       GPIOE
#define SD_DETECT_PIN        GPIO_PIN_15

/* 插卡时 PE15 的电平（实测后修正：0=插卡低电平，1=插卡高电平）。
 * 原理图 CDSW 机械开关 + R173 1.5M 偏置，文本层无法定极性，
 * 首次烧录后用任务里 SD_DETECT 日志实测插拔状态确定。 */
#define SD_DETECT_INSERTED_LEVEL  0U

/* SDIO 数据时钟 = SDIOCLK/(2+ClockDiv) = 48MHz/(2+2) = 12MHz
 * （f_getfree 大扫描在 24MHz 出现 FR_DISK_ERR，降频增强信号裕量） */
#define SDIO_CLK_DIV         6U   /* SDIO_CK=48/(2+6)=6MHz：12MHz 下 DMA 启动延迟致 TXUNDERR（写全丢），6MHz 缓冲余量翻倍 */
#define SD_TIMEOUT_MS        1000U

extern SD_HandleTypeDef hsd;
extern DMA_HandleTypeDef hdma_sdio_tx;   /* DMA2_Stream6_Ch4，SDIO 写数据用 */
extern DMA_HandleTypeDef hdma_sdio_rx;

/* ---- 接口 ---- */

/* SDIO 中断入口（stm32f4xx_it.c 的 SDIO_IRQHandler 转发到这里） */
void SD_SDIO_IRQHandler(void);
void SD_SDIO_DMA_TX_IRQHandler(void);
void SD_SDIO_DMA_RX_IRQHandler(void);

/* SDIO+GPIO+NVIC 初始化 + HAL_SD_Init（识别卡、升 4-bit 宽总线） */
HAL_StatusTypeDef SD_SDIO_Init(void);

/* 诊断：打印卡信息（类型/容量/CSD 写保护位），写失败时定位用 */
void SD_SDIO_DumpCardInfo(void);

/* 卡是否插入（按 SD_DETECT_INSERTED_LEVEL 判定） */
uint8_t SD_SDIO_CardInserted(void);

/* 读块（阻塞，内部带超时等待） */
HAL_StatusTypeDef SD_SDIO_ReadBlocks(uint8_t *buf, uint32_t blk_addr, uint32_t blk_cnt);

/* 写块（阻塞，内部带超时等待写完成） */
HAL_StatusTypeDef SD_SDIO_WriteBlocks(const uint8_t *buf, uint32_t blk_addr, uint32_t blk_cnt);

#endif /* __SD_SDIO_H */
