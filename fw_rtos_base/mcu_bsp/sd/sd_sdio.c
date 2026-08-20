/**
 * @file    sd_sdio.c
 * @brief   SD 卡 SDIO 底层驱动（HAL_SD 封装，4-bit 宽总线，24MHz）
 *
 * 对照 A 板原理图：SDIO_D0~D3=PC8~PC11、CK=PC12、CMD=PD2、检测=PE15。
 * 复用功能 AF12（GPIO_AF12_SDIO），数据/命令线内部上拉。
 * SDIOCLK=48MHz（PLLQ，与 USB 同源），SDIO_CK=24MHz（ClockDiv=0）。
 */
#include "sd_sdio.h"
#include "bsp_log.h"
#include "FreeRTOS.h"
#include "task.h"

SD_HandleTypeDef hsd;

/* 写路径步进标记（.bss，Keil Watch 看 g_sd_step 定位死锁点，RST 不清）：
 * 1=进入WriteBlocks_DMA 2=发CMD前 3=CMD返回 4=DMA启动前 5=DMA已启动
 * 6=DPSM配置完 7=HAL返回 8=等待READY循环 9=写完成 10=写测试全过 */
volatile uint32_t g_sd_step = 0U;
DMA_HandleTypeDef hdma_sdio_tx;   /* SDIO TX：DMA2_Stream6_Channel4 */
DMA_HandleTypeDef hdma_sdio_rx;   /* SDIO RX：DMA2_Stream3_Channel4（预留） */

/* HAL_SD_MspInit 回调：SDIO 时钟 + 引脚 + NVIC + DMA（HAL_SD_Init 内部自动调用） */
void HAL_SD_MspInit(SD_HandleTypeDef *hsd_handle)
{
    if (hsd_handle->Instance != SDIO)
    {
        return;
    }

    __HAL_RCC_SDIO_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOD_CLK_ENABLE();
    __HAL_RCC_GPIOE_CLK_ENABLE();
    __HAL_RCC_DMA2_CLK_ENABLE();

    /* D0~D3 + CK + CMD 全部 AF12(SDIO)，内部上拉（SD 规范要求） */
    GPIO_InitTypeDef gpio = {0};
    gpio.Mode  = GPIO_MODE_AF_PP;
    gpio.Pull  = GPIO_PULLUP;
    gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.Alternate = GPIO_AF12_SDIO;

    gpio.Pin = SD_D0_PIN | SD_D1_PIN | SD_D2_PIN | SD_D3_PIN | SD_CK_PIN;
    HAL_GPIO_Init(SD_GPIO_PORT, &gpio);

    gpio.Pin = SD_CMD_PIN;
    HAL_GPIO_Init(SD_CMD_PORT, &gpio);

    /* 卡检测脚：普通输入，内部无上下拉（外部 R173 1.5M 偏置） */
    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pull = GPIO_NOPULL;
    gpio.Alternate = 0U;
    gpio.Pin = SD_DETECT_PIN;
    HAL_GPIO_Init(SD_DETECT_PORT, &gpio);

    /* ---- SDIO DMA（轮询写会 TX_UNDERRUN，必须 DMA 供数） ----
     * F427 映射：TX=DMA2_Stream6_Channel4（MEM→外设），RX=DMA2_Stream3_Channel4 */
    hdma_sdio_tx.Instance                 = DMA2_Stream6;
    hdma_sdio_tx.Init.Channel             = DMA_CHANNEL_4;
    hdma_sdio_tx.Init.Direction           = DMA_MEMORY_TO_PERIPH;
    hdma_sdio_tx.Init.PeriphInc           = DMA_PINC_ENABLE;   /* SDIO FIFO 地址固定 */
    hdma_sdio_tx.Init.MemInc              = DMA_MINC_ENABLE;
    hdma_sdio_tx.Init.PeriphDataAlignment = DMA_PDATAALIGN_WORD;
    hdma_sdio_tx.Init.MemDataAlignment    = DMA_MDATAALIGN_WORD;
    hdma_sdio_tx.Init.Mode                = DMA_NORMAL;
    hdma_sdio_tx.Init.Priority            = DMA_PRIORITY_HIGH;
    hdma_sdio_tx.Init.FIFOMode            = DMA_FIFOMODE_ENABLE;
    hdma_sdio_tx.Init.FIFOThreshold       = DMA_FIFO_THRESHOLD_FULL;
    hdma_sdio_tx.Init.MemBurst            = DMA_MBURST_INC4;
    hdma_sdio_tx.Init.PeriphBurst         = DMA_PBURST_INC4;
    if (HAL_DMA_Init(&hdma_sdio_tx) != HAL_OK)
    {
        LOG_E("sd", "DMA2_S6 (SDIO TX) init failed");
    }
    __HAL_LINKDMA(hsd_handle, hdmatx, hdma_sdio_tx);

    /* RX DMA 预留（当前读走轮询，已验证稳定） */
    hdma_sdio_rx.Instance                 = DMA2_Stream3;
    hdma_sdio_rx.Init.Channel             = DMA_CHANNEL_4;
    hdma_sdio_rx.Init.Direction           = DMA_PERIPH_TO_MEMORY;
    hdma_sdio_rx.Init.PeriphInc           = DMA_PINC_ENABLE;
    hdma_sdio_rx.Init.MemInc              = DMA_MINC_ENABLE;
    hdma_sdio_rx.Init.PeriphDataAlignment = DMA_PDATAALIGN_WORD;
    hdma_sdio_rx.Init.MemDataAlignment    = DMA_MDATAALIGN_WORD;
    hdma_sdio_rx.Init.Mode                = DMA_NORMAL;
    hdma_sdio_rx.Init.Priority            = DMA_PRIORITY_HIGH;
    hdma_sdio_rx.Init.FIFOMode            = DMA_FIFOMODE_ENABLE;
    hdma_sdio_rx.Init.FIFOThreshold       = DMA_FIFO_THRESHOLD_FULL;
    hdma_sdio_rx.Init.MemBurst            = DMA_MBURST_INC4;
    hdma_sdio_rx.Init.PeriphBurst         = DMA_PBURST_INC4;
    if (HAL_DMA_Init(&hdma_sdio_rx) != HAL_OK)
    {
        LOG_E("sd", "DMA2_S3 (SDIO RX) init failed");
    }
    __HAL_LINKDMA(hsd_handle, hdmarx, hdma_sdio_rx);

    /* SDIO 中断：优先级 5（FreeRTOS 系统调用上限边界，合法） */
    HAL_NVIC_SetPriority(SDIO_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(SDIO_IRQn);

    /* DMA 中断：SDIO TX 完成依赖它置位状态 */
    HAL_NVIC_SetPriority(DMA2_Stream6_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(DMA2_Stream6_IRQn);
    HAL_NVIC_SetPriority(DMA2_Stream3_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(DMA2_Stream3_IRQn);
}

/* DMA 中断入口（stm32f4xx_it.c 调用）：TX 完成状态推进靠它 */
void SD_SDIO_DMA_TX_IRQHandler(void)
{
    HAL_DMA_IRQHandler(&hdma_sdio_tx);
}

void SD_SDIO_DMA_RX_IRQHandler(void)
{
    HAL_DMA_IRQHandler(&hdma_sdio_rx);
}

/* SDIO 中断处理入口（stm32f4xx_it.c 的 SDIO_IRQHandler 调用） */
void SD_SDIO_IRQHandler(void)
{
    HAL_SD_IRQHandler(&hsd);
}

/**
 * @brief SDIO + SD 卡完整初始化（阻塞）
 * @retval HAL_OK 卡识别成功（已升 4-bit 宽总线）
 */
HAL_StatusTypeDef SD_SDIO_Init(void)
{
    HAL_StatusTypeDef st;

    hsd.Instance = SDIO;
    hsd.Init.ClockEdge           = SDIO_CLOCK_EDGE_RISING;
    hsd.Init.ClockBypass         = SDIO_CLOCK_BYPASS_DISABLE;
    hsd.Init.ClockPowerSave      = SDIO_CLOCK_POWER_SAVE_DISABLE;
    hsd.Init.BusWide             = SDIO_BUS_WIDE_1B; /* 识别阶段 1-bit */
    hsd.Init.HardwareFlowControl = SDIO_HARDWARE_FLOW_CONTROL_DISABLE;
    hsd.Init.ClockDiv            = SDIO_CLK_DIV;     /* 数据时钟 24MHz */

    /* HAL_SD_Init 内部：低时钟识别卡 → 初始化 → 按 ClockDiv 提速 */
    st = HAL_SD_Init(&hsd);
    if (st != HAL_OK)
    {
        LOG_E("sd", "HAL_SD_Init failed (%d)", (int)st);
        return st;
    }

    /* 升 4-bit 宽总线（必须配合 HAL_SD_ConfigWideBusOperation） */
    if (HAL_SD_ConfigWideBusOperation(&hsd, SDIO_BUS_WIDE_4B) != HAL_OK)
    {
        LOG_E("sd", "4-bit wide bus switch failed");
        return HAL_ERROR;
    }

    LOG_I("sd", "SD init OK: 4-bit bus @ %luMHz",
          (unsigned long)(48U / (2U + (unsigned long)SDIO_CLK_DIV)));
    return HAL_OK;
}

uint8_t SD_SDIO_CardInserted(void)
{
    GPIO_PinState lv = HAL_GPIO_ReadPin(SD_DETECT_PORT, SD_DETECT_PIN);
    return (lv == GPIO_PIN_SET) ? (uint8_t)(SD_DETECT_INSERTED_LEVEL == 1U)
                                : (uint8_t)(SD_DETECT_INSERTED_LEVEL == 0U);
}

HAL_StatusTypeDef SD_SDIO_ReadBlocks(uint8_t *buf, uint32_t blk_addr, uint32_t blk_cnt)
{
    HAL_StatusTypeDef st = HAL_SD_ReadBlocks(&hsd, buf, blk_addr, blk_cnt, SD_TIMEOUT_MS);
    if (st == HAL_OK)
    {
        /* 等数据总线空闲（读完成） */
        uint32_t t = 0U;
        while ((HAL_SD_GetCardState(&hsd) != HAL_SD_CARD_TRANSFER) && (t < SD_TIMEOUT_MS))
        {
            vTaskDelay(1);
            t++;
        }
    }
    return st;
}

HAL_StatusTypeDef SD_SDIO_WriteBlocks(const uint8_t *buf, uint32_t blk_addr, uint32_t blk_cnt)
{
    /* A/B 实验：轮询写（v0.1.21 起）——DMA 写 TXUNDERR 写全丢，
     * 6MHz 下 FIFO 128B 缓冲 = 42µs 窗口，CPU 轮询填字（~0.6µs/32字）完全供得上。
     * 若轮询成功 → DMA 供数链路 bug；若仍 TXUNDERR → SDIO 数据路径问题。 */
    g_sd_step = 8U;
    LOG_D("sd", "wb-poll: pData=%p blk=%lu DCTRL=0x%lX",
          (void *)buf, (unsigned long)blk_addr, (unsigned long)SDIO->DCTRL);

    /* 轮询写期间关调度（taskENTER_CRITICAL）：CPU 供 FIFO 必须不间断。
     * 根因实锤 2026-08-20：SdCard 任务优先级 3 被 Led(5)/Oled(4) 抢占，
     * 抢占期间 FIFO 64B 缓冲 21µs 耗尽 → TXUNDERR → 卡丢整块。
     * 写 512B @6MHz ≈170µs 临界区，对系统影响可忽略。 */
    taskENTER_CRITICAL();
    HAL_StatusTypeDef st = HAL_SD_WriteBlocks(&hsd, (uint8_t *)buf, blk_addr, blk_cnt, SD_TIMEOUT_MS);
    taskEXIT_CRITICAL();
    if (st != HAL_OK)
    {
        LOG_E("sd", "HAL_SD_WriteBlocks(poll) FAIL blk=%lu cnt=%lu st=%d errcode=0x%lX",
              (unsigned long)blk_addr, (unsigned long)blk_cnt,
              (int)st, (unsigned long)hsd.ErrorCode);
        return st;
    }

    /* 轮询版内部已等 DATAEND；再等卡回 TRANSFER（编程完成） */
    uint32_t t2 = 0U;
    while ((HAL_SD_GetCardState(&hsd) != HAL_SD_CARD_TRANSFER) && (t2 < SD_TIMEOUT_MS))
    {
        vTaskDelay(1);
        t2++;
    }
    if (t2 >= SD_TIMEOUT_MS)
    {
        LOG_E("sd", "card busy wait timeout, state=%lu",
              (unsigned long)HAL_SD_GetCardState(&hsd));
        return HAL_TIMEOUT;
    }

    LOG_D("sd", "wb-poll-post: st=%d STA=0x%lX FIFOCNT=%lu err=0x%lX",
          (int)st, (unsigned long)SDIO->STA, (unsigned long)SDIO->FIFOCNT,
          (unsigned long)hsd.ErrorCode);

    g_sd_step = 9U;
    return HAL_OK;
}

/**
 * @brief 诊断：打印卡完整信息（类型/版本/容量/CSD 写保护位）
 *
 * CSD v2.0（SDHC/XC）低 16 位：bit13=PERM_WRITE_PROTECT、bit12=TMP_WRITE_PROTECT。
 * 写保护位置位时所有写命令会被卡拒绝——f_mkfs FR_DISK_ERR 的首查项。
 */
void SD_SDIO_DumpCardInfo(void)
{
    HAL_SD_CardInfoTypeDef *ci = &hsd.SdCard;
    uint32_t csd_hi = hsd.CSD[0];
    uint32_t csd_lo = hsd.CSD[1];

    LOG_I("sd", "card: type=%lu(1=SDSC 2=SDHC/XC) ver=%lu class=%lu blks=%lu blksz=%lu",
          (unsigned long)ci->CardType, (unsigned long)ci->CardVersion,
          (unsigned long)ci->Class, (unsigned long)ci->BlockNbr,
          (unsigned long)ci->BlockSize);
    LOG_I("sd", "csd_hi=0x%08lX csd_lo=0x%08lX perm_wp=%d tmp_wp=%d sdio_pwr=0x%lX",
          (unsigned long)csd_hi, (unsigned long)csd_lo,
          (int)((csd_lo >> 13) & 1U), (int)((csd_lo >> 12) & 1U),
          (unsigned long)SDIO->POWER);
}
