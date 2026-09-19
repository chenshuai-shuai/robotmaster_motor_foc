/*
 * disp_st7735s_spi.c - 驱动 B：1.8" ST7735S 彩屏（SPI1 + 全帧显存 + 脏矩形行带刷新）
 *
 * 依据：docs/2_方案（定稿设计）/设计_彩屏驱动与屏幕抽象层.md（D1–D7 定稿）
 *       docs/2_方案（定稿设计）/编码方案_彩屏驱动与屏幕抽象层.md §5（S2 主体）
 * 接线：SDI=PA7  SCL=PB3  CS=PB10(低=LCD/高=字库)  SDO=PA6  RS=PB9  BLK=3V3（A 板 OLED 口 7P）
 *
 * 三个必须记住的坑：
 *   ① **PB3 = JTDO**：F4 上没有 F1 的 SWJ_CFG 寄存器/宏，调试口占用是"AF0 复用"，
 *      把 PB3 配成 AF5(SPI1_SCK) 即摘掉调试功能；PA13/PA14(SWD) 全程不碰。
 *   ② **CS 反相双从机**：低=LCD、高=字库。显示事务全程 CS 低；读字库必须成对进出（见 .h 宏）。
 *   ③ **不许用 HAL_Delay / HAL_SPI_Transmit 的超时**：本文件在调度器启动前就会被调用，
 *      此时 uwTick 被 xTaskCreate 留下的 BASEPRI 屏蔽冻住（工程实测），任何"基于 tick 的等待"
 *      要么立刻超时要么永远不超时 → 全部延时用 DWT 忙等、全部等待用 DWT 限时自旋。
 *
 * 点灯期踩出来的四条（都在下面代码里留了防线）：
 *   · **凡改显存必标脏**（内部 fb_* 一样）：v0.1.69 的图案自检没标脏 → 开机那次 flush 是空操作
 *     → 屏从上电起从没被写过（现象=花屏/白屏 + "复位后要等一会才启动"）。
 *   · **推屏全绿 ≠ 面板收到了**：`pushes/flush/spifail` 只证明 MCU 的 SPI 发出去了。
 *     白屏排查的第一判据是 bus_check 的反相闪屏（判"面板收不收命令"），再谈像素与坐标。
 *   · **字库探测不许拿"首字节"当判据**：8x16 'A' 字模顶部是空行 → 首字节本来就 0x00，
 *     旧判据（byte0==0 即缺席）把好芯片误判成 absent；改成连读 16 字节按内容判，
 *     并顺手把它当"SPI 总线物理通不通"的证人（字库读得到 → SCK/MOSI/MISO 与 CS 高选通都好）。
 *   · **写路径只等 TXE**（不等 RXNE、限时只算一次）：改完后整屏 40960B = 31.3ms，正好 10.5MHz 线速。
 */
/* ★ 本文件被自己的宏整体包住（M3 文件级隔离）。**顺序陷阱**：#if 必须在 include 之后 ——
 *   `#if` 写在 include 之前时该宏尚未定义 → 预处理器按 0 处理 → 整个文件被**静默**编成空对象，
 *   编译期一声不响，直到链接期才报 `L6218E: Undefined symbol Disp_Init`（2026-09-19 实踩）。
 *   同一陷阱对 FEATURE_DISP_FONTS 派生宏、对任何"文件级隔离"文件都成立。 */
#include "feature_config.h"

#if FEATURE_DISP_ST7735S_SPI

#include "disp_port.h"
#include "disp_st7735s_spi.h"
#include "disp_geom.h"
#include "board_config.h"
#include "main.h"
#include "bsp_log.h"
#include "OLED_Data.h" /* 复用内置 ASCII 点阵（E1：6x8 / 8x16，免读 ROM、零延迟） */

/* 显存：128*160*2 = 40,960B（普通 .bss；MDK-ARM/F427IIH6_CAN.sct 的 RW_IRAM1 192KB 装得下，
 * 预算见编码方案 §12 —— 静态数组，不进 FreeRTOS heap） */
static uint16_t s_fb[DISP_ST7735S_W * DISP_ST7735S_H];

static SPI_HandleTypeDef s_spi;
static Disp_GeomRect_t s_dirty;

static Disp_Info_t s_info = {
    "ST7735S-SPI", DISP_ST7735S_W, DISP_ST7735S_H, 16u,
    { 6u, 8u, 8u }, { 8u, 16u, 16u }, /* SMALL=6x8；MED/BIG=8x16（BIG 的 12x24 字库大字属 S4） */
    0u, /* has_brightness（BLK 接 3V3，无调光） */
    0u  /* has_cn（板载字库读模 + 汉字渲染属 S4；S4 落地时置 1） */
};

static uint8_t s_inited;       /* 引脚/SPI 已配好（main 里完成，毫秒级） */
static uint8_t s_ready;         /* 屏已完成上电/初始化（显示任务里完成）→ 绘制才生效 */
static uint8_t s_bringup_done;  /* BringUp 只跑一次 */
static uint8_t s_spi_fail;   /* SPI 传输超时/初始化失败 → 自检 FAIL 并停止推屏 */
static uint8_t s_spi_fail_logged;
static uint8_t s_font_ok;    /* 字库芯片应答（自检 WARN 判据之一） */
static uint8_t s_font_byte;
static uint8_t s_flush_seen;
static uint32_t s_last_flush_us;
static uint32_t s_push_cnt;  /* 真正推屏次数（Disp_PushCount）：flush= 数值冻住时靠它区分"没变化"与"卡死" */
static uint32_t s_spi_limit; /* DWT 限时（周期数）：TXE/RXNE 等"标志"用它，100us 够（被抢占也不误判） */
static uint32_t s_busy_limit;/* DWT 限时（周期数）给 BSY 这种"量时长"的等待：**必须放宽**——
                              * 驱动搬进任务后会被高优先级任务（日志/监控）抢占，DWT 不暂停，
                              * 100us 预算一被抢占就假超时（2026-09-19 实踩：coltest 白帧误报
                              * SPI stalled(busy) → spifail=1 → 之后所有推屏被锁死、界面停在半幅）*/
static volatile uint32_t s_last_flush_ms;

/* ============================ DWT 微秒/毫秒忙等 ============================ */

static void disp_dwt_init(void)
{
    if ((DWT->CTRL & DWT_CTRL_CYCCNTENA_Msk) == 0U)
    {
        CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
        DWT->CYCCNT = 0U;
        DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    }
    s_spi_limit = 100u * (SystemCoreClock / 1000000U);  /* 100us：等标志（TXE/RXNE） */
    s_busy_limit = 500u * (SystemCoreClock / 1000U);    /* 500ms：量时长（BSY 落），容纳任务抢占 */
    if (s_spi_limit == 0u)
    {
        s_spi_limit = 100u;
    }
    if (s_busy_limit == 0u)
    {
        s_busy_limit = 100u;
    }
}

static uint32_t disp_cyc_to_us(uint32_t cyc)
{
    uint32_t per_us = SystemCoreClock / 1000000U;

    return (per_us == 0U) ? 0u : (cyc / per_us);
}

static void disp_delay_us(uint32_t us)
{
    uint32_t t0 = DWT->CYCCNT;
    uint32_t ticks = us * (SystemCoreClock / 1000000U);

    while ((uint32_t)(DWT->CYCCNT - t0) < ticks)
    {
    }
}

/* ============================ SPI 收发（限时自旋） ============================ */

/* 传输异常统一入口：置错 + 只打一条 E 日志（然后由上层停手，自检转 FAIL） */
static void spi_note_fail(const char *what)
{
    s_spi_fail = 1u;
    if (s_spi_fail_logged == 0u)
    {
        s_spi_fail_logged = 1u;
        LOG_E("disp", "SPI stalled (%s) -> display stopped (check wire/speed/CS)", what);
    }
}

/* 单字节**写**：只等 TXE（不依赖 RXNE —— 写路径读回数据没用，少一个失败面且满速）。
 * 单字节 @10.5MHz ≈ 0.76us，给 100us 兜底；超时置错并停手（绝不无限等待）。 */
static uint8_t spi_tx_byte(uint8_t b)
{
    uint32_t t0 = DWT->CYCCNT;

    while ((DISP_SPI->SR & SPI_SR_TXE) == 0u)
    {
        if ((uint32_t)(DWT->CYCCNT - t0) > s_spi_limit)
        {
            spi_note_fail("tx");
            return 0u;
        }
    }
    *(volatile uint8_t *)&DISP_SPI->DR = b;
    return 1u;
}

/* ★★ LCD 字节发送：**每字节进出一次 CS**（CS 只在 8 个 bit 期间为低=选中 LCD）。
 * 为什么必须这样（2026-09-19 实机定案）：本模块是"CS 反相双从机"（低=LCD / 高=字库），
 * **全程拉低 CS 时面板完全不应答**——推屏全绿、字库照读、RS 也对，屏就是一片白；
 * 改成"每字节脉冲 CS"后**同一份初始化与像素流立刻出画**（速率扫描 search#5 = 1.3MHz + 脉冲 CS
 * → 红屏+数字 5；search#1~4 = 全程低 CS → 全白）。这也正是商家例程的写法：
 *   LCD_WR_DATA8：LCD_CS_Clr() → 8 bit → LCD_CS_Set()（每字节一次）。
 * 注：**字库事务不能**用这个风格（字库芯片要求 CS 在整个事务内稳定），见 font_probe。 */
static void lcd_byte(uint8_t b)
{
    uint32_t tb;

    DISP_CS_SEL_LCD(); /* 字节开始：选中 LCD */
    (void)spi_tx_byte(b);
    tb = DWT->CYCCNT;
    while ((DISP_SPI->SR & SPI_SR_BSY) != 0u) /* 等这 8 bit 上完线（宽预算：容忍任务抢占） */
    {
        if ((uint32_t)(DWT->CYCCNT - tb) > s_busy_limit)
        {
            spi_note_fail("busy");
            break;
        }
    }
    DISP_CS_SEL_FONT(); /* 字节结束：CS 回空闲高（= 选中字库） */
}

/* 等最后一个字节真的上完线（BSY 落），并清掉接收残留（防 OVR 影响后续读字库） */
static void spi_wait_idle(void)
{
    uint32_t t0 = DWT->CYCCNT;

    while ((DISP_SPI->SR & SPI_SR_BSY) != 0u)
    {
        if ((uint32_t)(DWT->CYCCNT - t0) > s_busy_limit) /* 同上：量时长的等待用宽预算 */
        {
            spi_note_fail("busy");
            return;
        }
    }
    if ((DISP_SPI->SR & SPI_SR_RXNE) != 0u)
    {
        (void)*(volatile uint8_t *)&DISP_SPI->DR;
    }
}

/* 单字节**读**（只给读字库用）：发 dummy 再收 */
static uint8_t spi_rx_byte(uint8_t *ok)
{
    uint32_t t0;

    if (spi_tx_byte(0xFFu) == 0u)
    {
        *ok = 0u;
        return 0xFFu;
    }
    t0 = DWT->CYCCNT;
    while ((DISP_SPI->SR & SPI_SR_RXNE) == 0u)
    {
        if ((uint32_t)(DWT->CYCCNT - t0) > s_spi_limit)
        {
            spi_note_fail("rx");
            *ok = 0u;
            return 0xFFu;
        }
    }
    *ok = 1u;
    return *(volatile uint8_t *)&DISP_SPI->DR;
}

/* ============================ LCD 命令/数据 ============================ */

static void lcd_cmd(uint8_t c)
{
    DISP_RS_CMD();
    lcd_byte(c);
}

static void lcd_data(const uint8_t *d, uint32_t n)
{
    uint32_t i;

    DISP_RS_DATA();
    for (i = 0u; (i < n) && (s_spi_fail == 0u); i++)
    {
        lcd_byte(d[i]); /* 参数/像素一律"每字节脉冲 CS"（例程 LCD_WR_DATA 同款） */
    }
}

static void lcd_data1(uint8_t d)
{
    DISP_RS_DATA();
    lcd_byte(d);
}

/* 设显示窗口 + 进"写显存"模式（0x2C 之后的数据都进 GRAM） */
static void lcd_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    uint8_t b[4];

    b[0] = (uint8_t)((uint16_t)(x0 + DISP_ST7735S_XSTART) >> 8);
    b[1] = (uint8_t)((uint16_t)(x0 + DISP_ST7735S_XSTART) & 0xFFu);
    b[2] = (uint8_t)((uint16_t)(x1 + DISP_ST7735S_XSTART) >> 8);
    b[3] = (uint8_t)((uint16_t)(x1 + DISP_ST7735S_XSTART) & 0xFFu);
    lcd_cmd(0x2Au); /* CASET：列地址 */
    lcd_data(b, 4u);

    b[0] = (uint8_t)((uint16_t)(y0 + DISP_ST7735S_YSTART) >> 8);
    b[1] = (uint8_t)((uint16_t)(y0 + DISP_ST7735S_YSTART) & 0xFFu);
    b[2] = (uint8_t)((uint16_t)(y1 + DISP_ST7735S_YSTART) >> 8);
    b[3] = (uint8_t)((uint16_t)(y1 + DISP_ST7735S_YSTART) & 0xFFu);
    lcd_cmd(0x2Bu); /* RASET：行地址 */
    lcd_data(b, 4u);

    lcd_cmd(0x2Cu); /* RAMWR：接下来是像素数据（RGB565 大端） */
}

/* ============================ 初始化 ============================ */

static void panel_pins_init(void)
{
    GPIO_InitTypeDef g = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();

    /* ★ PB3=JTDO 的处置见文件头坑①：F4 直接配 AF5 即可（没有 SWJ_CFG 这个寄存器） */
    g.Mode = GPIO_MODE_AF_PP;
    g.Pull = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    g.Alternate = GPIO_AF5_SPI1;

    g.Pin = BRD_SCR_SPI_SCK_PIN;
    HAL_GPIO_Init(BRD_SCR_SPI_SCK_PORT, &g);
    g.Pin = BRD_SCR_SPI_MOSI_PIN;
    HAL_GPIO_Init(BRD_SCR_SPI_MOSI_PORT, &g);
    g.Pin = BRD_SCR_SPI_MISO_PIN;
    g.Pull = GPIO_PULLUP; /* 字库没被选中时 SDO 悬空 → 上拉防浮空噪声 */
    HAL_GPIO_Init(BRD_SCR_SPI_MISO_PORT, &g);

    /* CS 必须**先拉低**（选中 LCD）再配成输出：CS 高 = 选中字库芯片 */
    HAL_GPIO_WritePin(BRD_SCR_CS_PORT, BRD_SCR_CS_PIN, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(BRD_SCR_RS_PORT, BRD_SCR_RS_PIN, GPIO_PIN_RESET);
    g.Mode = GPIO_MODE_OUTPUT_PP;
    g.Pull = GPIO_NOPULL;
    g.Pin = (uint32_t)(BRD_SCR_CS_PIN | BRD_SCR_RS_PIN); /* CS=PB10 / RS=PB9，同口同配置 */
    HAL_GPIO_Init(BRD_SCR_CS_PORT, &g);
}

static void spi1_init(void)
{
    __HAL_RCC_SPI1_CLK_ENABLE();

    s_spi.Instance = DISP_SPI;
    s_spi.Init.Mode = SPI_MODE_MASTER;
    s_spi.Init.Direction = SPI_DIRECTION_2LINES; /* 保留 MISO：读字库要用 */
    s_spi.Init.DataSize = SPI_DATASIZE_8BIT;
    s_spi.Init.CLKPolarity = SPI_POLARITY_LOW; /* CPOL=0 */
    s_spi.Init.CLKPhase = SPI_PHASE_1EDGE;     /* CPHA=0 */
    s_spi.Init.NSS = SPI_NSS_SOFT;             /* SSM=1+SSI=1（HAL 写）→ 不会出 MODF */
    s_spi.Init.BaudRatePrescaler = DISP_SPI_BAUD_PRESCALER;
    s_spi.Init.FirstBit = SPI_FIRSTBIT_MSB;
    s_spi.Init.TIMode = SPI_TIMODE_DISABLE;
    s_spi.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
    s_spi.Init.CRCPolynomial = 10u;

    if (HAL_SPI_Init(&s_spi) != HAL_OK)
    {
        s_spi_fail = 1u;
        LOG_E("disp", "SPI1 init FAIL (need HAL_SPI_MODULE_ENABLED + stm32f4xx_hal_spi.c in project)");
        return;
    }
    __HAL_SPI_ENABLE(&s_spi);
}

/* 初始化序列：照抄模块官方例程 HARDWARE/LCD/lcd_init.c（值不许凭记忆改）*/
typedef struct
{
    uint8_t cmd;
    uint8_t n;
    uint8_t d[16];
    uint16_t delay_ms;
} St7735_Step_t;

static const St7735_Step_t s_init_seq[] = {
    { 0x01u, 0u, { 0u }, 120u },                                        /* SWRESET：模块无 RST 脚，软复位是唯一复位手段 */
    { 0x11u, 0u, { 0u }, 120u },                                        /* SLPOUT */
    { 0xB1u, 3u, { 0x05u, 0x3Cu, 0x3Cu }, 0u },                         /* FRMCTR1 */
    { 0xB2u, 3u, { 0x05u, 0x3Cu, 0x3Cu }, 0u },                         /* FRMCTR2 */
    { 0xB3u, 6u, { 0x05u, 0x3Cu, 0x3Cu, 0x05u, 0x3Cu, 0x3Cu }, 0u },    /* FRMCTR3 */
    { 0xB4u, 1u, { 0x03u }, 0u },                                       /* INVCTR：点反相 */
    { 0xC0u, 3u, { 0x28u, 0x08u, 0x04u }, 0u },                         /* PWCTR1 */
    { 0xC1u, 1u, { 0xC0u }, 0u },                                       /* PWCTR2 */
    { 0xC2u, 2u, { 0x0Du, 0x00u }, 0u },                                /* PWCTR3 */
    { 0xC3u, 2u, { 0x8Du, 0x2Au }, 0u },                                /* PWCTR4 */
    { 0xC4u, 2u, { 0x8Du, 0xEEu }, 0u },                                /* PWCTR5 */
    { 0xC5u, 1u, { 0x1Au }, 0u },                                       /* VMCTR1 */
    { 0x36u, 1u, { DISP_ST7735S_MADCTL }, 0u },                         /* MADCTL：竖屏正向 */
    { 0xE0u, 16u, { 0x04u, 0x22u, 0x07u, 0x0Au, 0x2Eu, 0x30u, 0x25u, 0x2Au,
                    0x28u, 0x26u, 0x2Eu, 0x3Au, 0x00u, 0x01u, 0x03u, 0x13u }, 0u }, /* GMCTRP1 */
    { 0xE1u, 16u, { 0x04u, 0x16u, 0x06u, 0x0Du, 0x2Du, 0x26u, 0x23u, 0x27u,
                    0x27u, 0x25u, 0x2Du, 0x3Bu, 0x00u, 0x01u, 0x04u, 0x13u }, 0u }, /* GMCTRN1 */
    { 0x3Au, 1u, { 0x05u }, 0u },                                       /* COLMOD：65k（RGB565） */
    { 0x29u, 0u, { 0u }, 0u },                                          /* DISPON */
};

static void panel_init_seq(void)
{
    uint32_t i;

    for (i = 0u; i < (sizeof(s_init_seq) / sizeof(s_init_seq[0])); i++)
    {
        lcd_cmd(s_init_seq[i].cmd);
        if (s_init_seq[i].n != 0u)
        {
            lcd_data(s_init_seq[i].d, (uint32_t)s_init_seq[i].n);
        }
        if (s_init_seq[i].delay_ms != 0u)
        {
            disp_delay_us((uint32_t)s_init_seq[i].delay_ms * 1000u);
        }
    }
    spi_wait_idle();
}

/* 字库芯片存在性探测（**顺带验证 CS 事务成对**）：读 ROM 里 'A' 的第一个字节。
 * 全 0x00 / 全 0xFF → 判字库不可用（S4 汉字能力据此决定），只打一条日志不死等。
 *
 * ★ 读之前必须先**清接收残留**：本驱动的写路径只等 TXE、不读 DR（见 spi_tx_byte），
 *   发完 4 个地址字节后 RXNE 里躺着的是"地址阶段收到的那一个字节"（且 OVR 已置位）。
 *   不清就直接读 → 读到的是残留字节，`font=` 判定毫无意义（v0.1.69 实测 byte0=0x00 就是这么来的）。 */
static void spi_drain_rx(void)
{
    if ((DISP_SPI->SR & SPI_SR_RXNE) != 0u)
    {
        (void)*(volatile uint8_t *)&DISP_SPI->DR;
    }
    (void)DISP_SPI->SR; /* F4 清 OVR：读 DR 后再读 SR */
}

static void font_probe(void)
{
    uint32_t a = DISP_FONT_ASCII_BASE_8X16 +
                 ((uint32_t)((uint8_t)DISP_FONT_PROBE_CH - DISP_FONT_ASCII_FIRST_CH) * DISP_FONT_ASCII_N_8X16);

    uint8_t ok = 1u;
    uint8_t buf[16];
    uint8_t nz = 0u;
    uint8_t i;

    spi_wait_idle();
    DISP_CS_SEL_FONT(); /* 进事务 */
    (void)spi_tx_byte(DISP_FONT_CMD_READ);
    (void)spi_tx_byte((uint8_t)(a >> 16));
    (void)spi_tx_byte((uint8_t)(a >> 8));
    (void)spi_tx_byte((uint8_t)a);
    spi_wait_idle(); /* 等地址发完（BSY 落） */
    spi_drain_rx();  /* ★ 清掉地址阶段的残留字节，否则下面读到的是它 */
    for (i = 0u; i < (uint8_t)sizeof(buf); i++)
    {
        buf[i] = spi_rx_byte(&ok); /* 连读（字库芯片地址自动递增） */
        if (buf[i] != 0x00u)
        {
            nz++;
        }
        if (ok == 0u)
        {
            break;
        }
    }
    DISP_CS_SEL_LCD(); /* ★ 出事务：必须把 CS 拉回 LCD（漏了 = 之后数据写进字库 → 花屏） */
    spi_wait_idle();
    spi_drain_rx();

    /* ★ 判据是"**16 字节里有没有内容**"，不是"第 1 字节非零"：
     *   ASCII 8x16 'A' 字模顶部本来就是空行 → 首字节 0x00 是**正常数据**，
     *   旧判据（byte0==0 即缺席）把好芯片误判成 absent（2026-09-19 实测 byte0=0x00 就是这么来的）。
     *   全 0x00 = 线被拉死/没人驱动；全 0xFF = 悬空。
     *   这一项同时是"**SPI 总线物理通不通**"的证人：字库读得到 → SCK/MOSI/MISO 与"CS 高选字库"都好。 */
    s_font_byte = buf[0];
    s_font_ok = ((nz >= 2u) && (ok != 0u) && (s_spi_fail == 0u)) ? 1u : 0u;
    LOG_I("disp", "font rom probe: %s (head %02X %02X %02X %02X, nonzero %u/16)",
          (s_font_ok != 0u) ? "OK" : "absent", (unsigned)buf[0], (unsigned)buf[1], (unsigned)buf[2],
          (unsigned)buf[3], (unsigned)nz);
}

/* ============================ 显存原语（内部，不裁剪） ============================ */

static void fb_put(int16_t x, int16_t y, uint16_t c)
{
    if ((x < 0) || (y < 0) || (x >= (int16_t)DISP_ST7735S_W) || (y >= (int16_t)DISP_ST7735S_H))
    {
        return; /* 契约：越界自动裁剪（像素级） */
    }
    s_fb[((uint32_t)y * DISP_ST7735S_W) + (uint16_t)x] = c;
}

static void fb_fill(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t c)
{
    int16_t i;
    int16_t j;

    for (j = y; j < (int16_t)(y + h); j++)
    {
        for (i = x; i < (int16_t)(x + w); i++)
        {
            fb_put(i, j, c);
        }
    }
}

static void line_plot(int16_t x, int16_t y, void *ctx)
{
    fb_put(x, y, *(const uint16_t *)ctx);
}

static void fb_line(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t c)
{
    Disp_GeomLine(x0, y0, x1, y1, line_plot, &c);
}

/* 取字模指针：内置点阵（E1 决策）＝ OLED_F6x8 / OLED_F8x16，索引 ch-0x20 */
static const uint8_t *glyph_of(char ch, Disp_Font_e sz, uint8_t *w_out, uint8_t *h_out)
{
    uint8_t idx = (uint8_t)ch;

    if ((idx < 0x20u) || (idx > 0x7Eu))
    {
        idx = (uint8_t)'?'; /* 不可打印字符统一画 '?'（防字模数组越界） */
    }
    idx = (uint8_t)(idx - 0x20u);

    if (sz == DISP_FONT_SMALL)
    {
        *w_out = 6u;
        *h_out = 8u;
        return OLED_F6x8[idx];
    }
    *w_out = 8u;
    *h_out = 16u;
    return OLED_F8x16[idx];
}

/* 画一个字符：纵向 8 点/字节、LSB 在上（disp_geom.h 的取位函数） */
static void fb_char(int16_t x, int16_t y, char ch, uint16_t fg, uint16_t bg, Disp_Font_e sz)
{
    const uint8_t *g;
    uint8_t w = 0u;
    uint8_t h = 0u;
    uint8_t pages;
    uint8_t page;
    uint8_t col;

    g = glyph_of(ch, sz, &w, &h);
    if (g == NULL)
    {
        return;
    }
    pages = (uint8_t)((h + 7u) / 8u);

    for (page = 0u; page < pages; page++)
    {
        for (col = 0u; col < w; col++)
        {
            uint8_t cb = g[((uint32_t)page * w) + col];
            uint8_t r;

            for (r = 0u; r < 8u; r++)
            {
                fb_put((int16_t)(x + col), (int16_t)((y + (page * 8u)) + r),
                       (Disp_GeomGlyphBit(cb, r) != 0u) ? fg : bg);
            }
        }
    }
}

static void fb_str(int16_t x, int16_t y, const char *s, uint16_t fg, uint16_t bg, Disp_Font_e sz)
{
    uint8_t w = (sz == DISP_FONT_SMALL) ? 6u : 8u;
    int16_t i;

    for (i = 0; (s != NULL) && (s[i] != '\0'); i++)
    {
        fb_char((int16_t)(x + (i * w)), y, s[i], fg, bg, sz);
    }
}

/* ============================ 刷新（脏矩形 → 行带） ============================ */

static void push_band(uint16_t y0, uint16_t y1)
{
    uint16_t y;

    lcd_set_window(0u, y0, (uint16_t)(DISP_ST7735S_W - 1u), y1);
    /* ★★ 关键：lcd_set_window() 的最后一步是 lcd_cmd(0x2C)，**它把 RS 留在"命令"态**。
     *    RS/DC 是**状态**不是每字节参数 —— 这里必须显式切回"数据"态，否则整帧像素会被面板
     *    当成命令吃掉（0xF8/0x00… 都是无效命令），GRAM 一个字节都写不进 →
     *    现象 = **整片白、颜色永不变**，而 `pushes=`/`flush=`/`spifail=` 全绿、字库照样能读
     *    （字库没有 RS 脚，查不出这个问题）。2026-09-19 实机踩过，别删这一行。 */
    DISP_RS_DATA();
    for (y = y0; (y <= y1) && (s_spi_fail == 0u); y++)
    {
        const uint16_t *row = &s_fb[(uint32_t)y * DISP_ST7735S_W];
        uint16_t x;

        for (x = 0u; x < DISP_ST7735S_W; x++)
        {
            lcd_byte((uint8_t)(row[x] >> 8));       /* RGB565 高字节在前 */
            lcd_byte((uint8_t)(row[x] & 0x00FFu));
            if (s_spi_fail != 0u)
            {
                break;
            }
        }
    }
    spi_wait_idle();
}

/* 只推脏区覆盖到的行带；CS 全程保持低（事务内不许抬高，否则数据被写进字库芯片） */
static void flush_dirty_now(void)
{
    uint32_t t0;
    uint16_t band;

    if ((s_dirty.valid == 0u) || (s_spi_fail != 0u))
    {
        return;
    }

    t0 = DWT->CYCCNT;
    for (band = 0u; band < DISP_ST7735S_H; band = (uint16_t)(band + DISP_ST7735S_BAND_H))
    {
        uint16_t by0 = band;
        uint16_t by1 = (uint16_t)(band + DISP_ST7735S_BAND_H - 1u);

        if (by1 > (uint16_t)(DISP_ST7735S_H - 1u))
        {
            by1 = (uint16_t)(DISP_ST7735S_H - 1u);
        }
        if (((int16_t)by1 < s_dirty.y0) || ((int16_t)by0 > s_dirty.y1))
        {
            continue; /* 该行带与脏区不相交 */
        }
        push_band(by0, by1);
        if (s_spi_fail != 0u)
        {
            break;
        }
    }
    s_last_flush_us = disp_cyc_to_us((uint32_t)(DWT->CYCCNT - t0));
    s_push_cnt++;
    Disp_GeomReset(&s_dirty);
}

/* ============================ 点灯期管脚/面板回读（白屏定位） ============================
 * 为什么还要这两个探针：字库芯片（`font=1`）能证明 SCK/MOSI/MISO 与 CS 线，
 * 但**字库芯片没有 DC/RS 脚** —— RS 是它唯一证明不了的一根线，而 RS 坏掉的表现正合：
 *   RS 卡在低 → 每个字节都被当命令 → 初始化/反相都能过，**像素永远写不进去** → 整片白。
 * 这两个探针把"MCU 侧焊盘"和"面板侧是否真在收命令"分开证： */
static void pin_selftest(void)
{
    uint8_t hi;
    uint8_t lo;

    /* 输出脚读 IDR = 读**焊盘实际电平** → 能抓"短到地/被外部强驱"（悬空/断线读不出来，那要靠万用表）。 */
    DISP_RS_DATA(); /* PB9 = 1 */
    disp_delay_us(5u);
    hi = (HAL_GPIO_ReadPin(BRD_SCR_RS_PORT, BRD_SCR_RS_PIN) == GPIO_PIN_SET) ? 1u : 0u;
    DISP_RS_CMD(); /* PB9 = 0 */
    disp_delay_us(5u);
    lo = (HAL_GPIO_ReadPin(BRD_SCR_RS_PORT, BRD_SCR_RS_PIN) == GPIO_PIN_RESET) ? 1u : 0u;
    LOG_I("disp", "pincheck RS(PB9): drv1->rd1 %u, drv0->rd0 %u (0 = shorted/fought)", (unsigned)hi,
          (unsigned)lo);

    DISP_CS_SEL_FONT(); /* PB10 = 1 */
    disp_delay_us(5u);
    hi = (HAL_GPIO_ReadPin(BRD_SCR_CS_PORT, BRD_SCR_CS_PIN) == GPIO_PIN_SET) ? 1u : 0u;
    DISP_CS_SEL_LCD(); /* PB10 = 0 */
    disp_delay_us(5u);
    lo = (HAL_GPIO_ReadPin(BRD_SCR_CS_PORT, BRD_SCR_CS_PIN) == GPIO_PIN_RESET) ? 1u : 0u;
    LOG_I("disp", "pincheck CS(PB10): drv1->rd1 %u, drv0->rd0 %u (0 = shorted/fought)", (unsigned)hi,
          (unsigned)lo);

    DISP_RS_CMD(); /* 收尾：RS 归命令态 */
}

/* 面板回读 RDDID(0x04)：要"CS 低选中 + 命令字节到 + 读回来"三件事同时成立才应答。
 * 面板能应答 = 面板真被选中、真在收命令、真在驱动 MISO —— 那"白屏且彩色不上屏"就只剩 RS 卡死/
 * 断线一条（读回的数据阶段不看 DC，所以 RS 卡低也照样能应答，正是我们要的区分度）。 */
static void panel_read_probe(void)
{
    uint8_t ok = 1u;
    uint8_t b[4];
    uint8_t nz = 0u;
    uint8_t i;

    spi_wait_idle();
    DISP_CS_SEL_LCD();
    lcd_cmd(0x04u); /* RDDID */
    spi_wait_idle();
    DISP_RS_DATA(); /* 读回阶段：面板驱动 SDO */
    spi_drain_rx(); /* 清命令阶段的接收残留 */
    for (i = 0u; i < 4u; i++)
    {
        b[i] = spi_rx_byte(&ok);
        if (b[i] != 0x00u)
        {
            nz++;
        }
        if (ok == 0u)
        {
            break;
        }
    }
    DISP_RS_CMD();
    spi_wait_idle();
    spi_drain_rx();

    LOG_I("disp", "panel RDDID: %02X %02X %02X %02X (nonzero %u/4; all 00/FF = no answer)",
          (unsigned)b[0], (unsigned)b[1], (unsigned)b[2], (unsigned)b[3], (unsigned)nz);
}

/* 反相闪屏（判"面板到底收不收命令"）：
 * 白屏 = 面板从没被初始化过。而"推屏成功（pushes/flush/spifail 全绿）"只证明 **MCU 的 SPI 发出去了**，
 * 不证明面板收到。0x21/0x20（INVON/INVOFF）**不改显存、只翻显示极性** → 连空白屏都会整片黑白翻转，
 * 肉眼一眼可见：
 *   · 闪 = 命令通路通（CS 选中 + RS/DC + SCK/MOSI 都对）→ 病在像素数据/面板配置
 *   · 不闪 = 面板根本没收到命令（CS 没选中 / RS 悬空 / SDI·SCL 接反 / 面板坏）
 * 两种 CS 极性各试一次，兜底"模块批次与接线表相反"：
 *   CS=LOW 段闪 **1** 次（与接线表一致）；CS=HIGH 段闪 **2** 次（模块是反的 → 把 .h 两个 CS 宏对调）。 */
static void bus_check(void)
{
    /* ⓪ 先"叫醒"：只补"让显示亮起来"必需的几条 + 彩色体检依赖的两条设置（不做整段初始化，省 240ms）。
     *    不能只发 SWRESET：软复位会把 MADCTL/COLMOD 打回默认，后面彩色体检的 16 位色就错了。 */
    DISP_CS_SEL_LCD();
    lcd_cmd(0x11u); /* SLPOUT */
    spi_wait_idle();
    disp_delay_us(120000u);
    lcd_cmd(0x29u); /* DISPON（反相必须"显示已开"才看得见） */
    spi_wait_idle();
    lcd_cmd(0x36u); /* MADCTL 补回 */
    spi_wait_idle();
    {
        const uint8_t mad = (uint8_t)DISP_ST7735S_MADCTL;
        const uint8_t com = 0x05u; /* 65k RGB565 */

        lcd_data(&mad, 1u);
        lcd_cmd(0x3Au); /* COLMOD 补回 */
        lcd_data(&com, 1u);
    }
    spi_wait_idle();
    disp_delay_us(100000u);

    /* ① CS=LOW（接线表的约定：低=LCD）→ 期望闪 1 次（半周期 400ms，够肉眼看）
     *   CS=高 那段（2 闪）已删：字库芯片在 CS 高时回了真数据，**"模块 CS 反相"已被当场否掉**（见 §10 判据表）。 */
    lcd_cmd(0x21u); /* INVON */
    spi_wait_idle();
    disp_delay_us((uint32_t)DISP_ST7735S_BUSCHECK_MS * 1000u);
    lcd_cmd(0x20u); /* INVOFF */
    spi_wait_idle();
    disp_delay_us((uint32_t)DISP_ST7735S_BUSCHECK_MS * 1000u);

    LOG_I("disp", "buscheck: 1 blink at CS=LOW (INVON); 0 blinks = panel got no command at all");
}

/* ============================ 网络体检（机器可读，不靠肉眼） ============================
 * 为什么要有它：屏对 MCU 是**只写设备** —— 模块引出的 SDO 是**字库芯片**的输出（我们对面板
 * 发 RDDID 实测回 `FF FF FF FF` = 那根线上没人应答，面板的读通路没引出来）→ "屏到底亮没亮"
 * 在软件里没有观测通道。能把结论做成日志的只有**电气层**：这个网络上有外部偏置吗？
 *   ① 输出驱动**低** → 立刻切"输入+无上下拉" → 等 100us → 读：
 *      **读到高 = 该网络有外部上拉**（浮空的脚会保持低电平——只有外部上拉能在 100us 内把它拉起来）
 *   ② 输出驱动**高** → 同样释放 → 读：**读到低 = 该网络有外部下拉**
 * 三个被测量，**两个是已知对照**（自证式，避免把噪声当结论）：
 *   · PB10 = 屏 CS：**已证接在模块上**（字库芯片读得出来）→ 它 = "模块上一个普通输入脚"的基准；
 *   · PB9  = 屏 RS：**嫌犯**。若它出现"外部上拉"——因为模块的 BLK 内部上拉到 VDD（文档实测过），
 *     这正是"**RS 线接在 BLK 脚上**"的机器可读证据；
 *     若它与 CS 表现一致（无偏置）→ 线上没有偏置源：要么是面板 DC 这种普通输入（那就该能工作）、
 *     要么线断/落在空脚上 → 用万用表量"一对脚"定案（见测试手册 TM-D1）。 */
static void net_bias(GPIO_TypeDef *port, uint16_t pin, uint8_t *pull_up, uint8_t *pull_dn)
{
    GPIO_InitTypeDef g = {0};

    g.Pin = pin;
    g.Pull = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_VERY_HIGH;

    /* ① 驱动低 → 释放 → 被拉高 = 有外部上拉 */
    g.Mode = GPIO_MODE_OUTPUT_PP;
    HAL_GPIO_Init(port, &g);
    HAL_GPIO_WritePin(port, pin, GPIO_PIN_RESET);
    g.Mode = GPIO_MODE_INPUT;
    HAL_GPIO_Init(port, &g);
    disp_delay_us(100u);
    *pull_up = (HAL_GPIO_ReadPin(port, pin) == GPIO_PIN_SET) ? 1u : 0u;

    /* ② 驱动高 → 释放 → 被拉低 = 有外部下拉 */
    g.Mode = GPIO_MODE_OUTPUT_PP;
    HAL_GPIO_Init(port, &g);
    HAL_GPIO_WritePin(port, pin, GPIO_PIN_SET);
    g.Mode = GPIO_MODE_INPUT;
    HAL_GPIO_Init(port, &g);
    disp_delay_us(100u);
    *pull_dn = (HAL_GPIO_ReadPin(port, pin) == GPIO_PIN_RESET) ? 1u : 0u;
}

static void net_probe(void)
{
    uint8_t pu = 0u;
    uint8_t pd = 0u;
    GPIO_InitTypeDef g = {0};

    net_bias(BRD_SCR_CS_PORT, BRD_SCR_CS_PIN, &pu, &pd); /* 对照：已知接在模块上 */
    LOG_I("disp", "netprobe CS(PB10,known-on-module): extPullUp=%u extPullDown=%u", (unsigned)pu,
          (unsigned)pd);

    net_bias(BRD_SCR_RS_PORT, BRD_SCR_RS_PIN, &pu, &pd); /* 嫌犯 */
    LOG_I("disp", "netprobe RS(PB9): extPullUp=%u extPullDown=%u (extPullUp=1 => RS wire is on BLK)",
          (unsigned)pu, (unsigned)pd);

    /* 还原：CS/RS 回推挽输出（显示逻辑依赖它）。
     * ★ 探针只驱动"已知是模块输入"的脚 —— 不要拿外部电路未知的脚（如按键）做对照：
     *   若那脚被别的电路有源驱动，我们驱高/驱低就是在跟它抢电流（可能数十 mA）。 */
    g.Pull = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    g.Mode = GPIO_MODE_OUTPUT_PP;
    g.Pin = (uint32_t)(BRD_SCR_CS_PIN | BRD_SCR_RS_PIN);
    HAL_GPIO_Init(BRD_SCR_CS_PORT, &g);
    DISP_CS_SEL_LCD();
    DISP_RS_CMD();

}

/* ============================ 点灯期搜索（一次烧录试完剩余全部假设） ============================
 * 剩下的可能性只有两类，这版把它们全试掉：
 *   A) **软件/配置层**：① SPI 速率（10.5MHz 是面板规格上限，杜邦线上可能被振铃破坏 → 初始化字节
 *      进不对 → 面板没进 display on → **任何像素都看不见、屏一直白**）；② CS 风格（例程是**每字节
 *      脉冲** CS，我们全程拉低）。→ 4 档速率 + 例程式 CS 各来一帧"红屏 + 大号档位数字"，
 *      **哪一档出红屏就把速率/风格定死在哪一档**（屏上数字就是档位号，不靠时间判断）。
 *   B) **电气层**：RS 到底有没有到模块。→ 最后两段**静态**电平窗口（RS 高 6s / 低 6s），
 *      红灯用两种不同频率闪当"现在哪一段"的信标，你用万用表量 **PB9 与模块第7脚**的电压。
 * 判据（四组数字）：PB9 高/低都对、模块脚也跟着 —— 线没问题（那就是模块/玻璃的事）；
 *   PB9 会变但模块脚不动 —— **就在那一段断**（重压端子或用空闲 IO 拉跳线）。
 * 只在点灯期用；屏正常后 DISP_ST7735S_DIAG_SEARCH 设 0u 关掉。 */
/* ============================ 偏移标定扫描（点灯期；把白边/错位一次定死） ============================
 * 为什么需要：ST7735S 不同批次玻璃的**可见区起点**不同（常见 0/0、0/1、1/0、2/1、2/3、0/2）。
 * 我们没有面板读回通道，唯一办法是"把候选逐档画满，看哪一档没有残边"。做法对每一档：
 *   ① `ggram_fill(黑)`：把**整块 GRAM（132x162）**都写黑 —— 不管可见区起点在哪，它一定被覆盖到，
 *      于是"没被写到的区域"不再显示面板上电默认的**白**（这正是用户看到的"最底下一条白线"）；
 *   ② 用**候选偏移**把可见窗口(0..127,0..159)填成**该档专属颜色** → 偏移正确的那档 = 整屏纯色无黑边；
 *      偏移不对的档 = 那一条边上留一条**黑边**（错几行就多宽），一眼可辨。
 * 颜色→档位：1=红 2=绿 3=蓝 4=黄 5=青 6=品红。用户只需回报"哪一档是整屏纯色、没有黑边"。 */
static const int8_t s_off_tab[6][2] = {
    { 0, 0 }, { 0, 1 }, { 1, 0 }, { 2, 1 }, { 2, 3 }, { 0, 2 },
};
static const uint16_t s_off_col[6] = {
    0xF800u, 0x07E0u, 0x001Fu, 0xFFE0u, 0x07FFu, 0xF81Fu, /* 红 绿 蓝 黄 青 品红 */
};

/* 窗口 + **显式偏移**（标定用；正式路径用编译期 DISP_ST7735S_XSTART/YSTART） */
static void lcd_set_window_off(int16_t xs, int16_t ys, uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    uint8_t b[4];
    uint16_t v;

    lcd_cmd(0x2Au);
    v = (uint16_t)(x0 + (uint16_t)xs); b[0] = (uint8_t)(v >> 8); b[1] = (uint8_t)v;
    v = (uint16_t)(x1 + (uint16_t)xs); b[2] = (uint8_t)(v >> 8); b[3] = (uint8_t)v;
    lcd_data(b, 4u);
    lcd_cmd(0x2Bu);
    v = (uint16_t)(y0 + (uint16_t)ys); b[0] = (uint8_t)(v >> 8); b[1] = (uint8_t)v;
    v = (uint16_t)(y1 + (uint16_t)ys); b[2] = (uint8_t)(v >> 8); b[3] = (uint8_t)v;
    lcd_data(b, 4u);
    lcd_cmd(0x2Cu);
}

/* 把**整块 GRAM**（132x162）刷成某色：保证可见区（无论起点在哪）被完全覆盖 */
static void ggram_fill(uint16_t color)
{
    uint32_t n = (uint32_t)DISP_ST7735S_GRAM_W * (uint32_t)DISP_ST7735S_GRAM_H;
    uint32_t i;

    lcd_set_window_off(0, 0, 0u, 0u, (uint16_t)(DISP_ST7735S_GRAM_W - 1u),
                       (uint16_t)(DISP_ST7735S_GRAM_H - 1u));
    DISP_RS_DATA();
    for (i = 0u; (i < n) && (s_spi_fail == 0u); i++)
    {
        lcd_byte((uint8_t)(color >> 8));
        lcd_byte((uint8_t)(color & 0x00FFu));
    }
    spi_wait_idle();
}

/* 用候选偏移填可见窗口 */
static void fill_win_off(int16_t xs, int16_t ys, uint16_t color)
{
    uint32_t n = (uint32_t)DISP_ST7735S_W * (uint32_t)DISP_ST7735S_H;
    uint32_t i;

    lcd_set_window_off(xs, ys, 0u, 0u, (uint16_t)(DISP_ST7735S_W - 1u), (uint16_t)(DISP_ST7735S_H - 1u));
    DISP_RS_DATA();
    for (i = 0u; (i < n) && (s_spi_fail == 0u); i++)
    {
        lcd_byte((uint8_t)(color >> 8));
        lcd_byte((uint8_t)(color & 0x00FFu));
    }
    spi_wait_idle();
}

static void offset_sweep(void)
{
    uint8_t k;

    for (k = 0u; k < 6u; k++)
    {
        ggram_fill(0x0000u); /* 整块 GRAM 黑：任何没被写到的可见区都显示黑，而不是上电默认的白 */
        fill_win_off(s_off_tab[k][0], s_off_tab[k][1], s_off_col[k]);
        LOG_I("disp", "offset#%u: xs=%d ys=%d -> report if THIS color filled the screen with no black edge",
              (unsigned)(k + 1u), (int)s_off_tab[k][0], (int)s_off_tab[k][1]);
        disp_delay_us(800000u);
    }
    /* 收尾：整块 GRAM 黑（后续彩色体检/界面即使有偏移，边条也只是黑，不再是白） */
    ggram_fill(0x0000u);
    LOG_I("disp", "offset sweep done: tell me which offset# was a full clean fill");
}

/* ★ 连线体检（电容法，纯 log，最后一个判据）：
 * 问题：pincheck 只能抓"短到地"，**抓不到"线断了"**——断线时焊盘仍归我们驱动，测起来一切正常。
 * 做法：同一条内部弱上拉（≈40kΩ）下，测量"驱低放掉电荷 → 充到读为高"所花的 **CPU 周期数**。
 *   τ = R_pullup × C_net：**空脚 C≈5pF**，接一段杜邦线 + 模块的 CMOS 输入 ≈ 30~150pF
 *   → 两者差 6~30 倍，而 CYCCNT 分辨率 6ns、计时循环约 4 周期 → 完全分得开。
 * 对照：**PA7 = MOSI 是"已证接在模块上"的同类线**（字库读得出来 → 它必然接着模块的 SDI 输入）
 *   → 它的读数 = "一根连着模块的线"的基准；PA6 = MISO（接字库 SDO）作为第二个基准。
 * 判据：
 *   · RS ≈ MOSI/MISO  → RS 线确实接着模块上的东西（电容来自线缆+输入）；
 *   · RS ≪ MOSI（接近空脚）→ **RS 线是断的/没压进端子**（线走线没问题、万用表量"两端口"也许还通，
 *     但**到模块那一端没接上**）→ 拉跳线或重压端子。
 * 注意：本探针会临时把 MOSI/MISO 从 AF 切回 GPIO，结束后必须还原成 AF5(SPI1)。 */
static uint32_t charge_cyc(GPIO_TypeDef *port, uint16_t pin, uint32_t mask)
{
    GPIO_InitTypeDef g = {0};
    uint32_t t0;

    g.Pin = pin;
    g.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    g.Pull = GPIO_NOPULL;

    g.Mode = GPIO_MODE_OUTPUT_PP; /* 驱低，把电荷放掉 */
    HAL_GPIO_Init(port, &g);
    HAL_GPIO_WritePin(port, pin, GPIO_PIN_RESET);
    disp_delay_us(50u);

    g.Mode = GPIO_MODE_INPUT; /* 切"输入+上拉"，计时到读为高 */
    g.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(port, &g);

    t0 = DWT->CYCCNT;
    while ((port->IDR & mask) == 0u)
    {
        if ((uint32_t)(DWT->CYCCNT - t0) > (SystemCoreClock / 50u)) /* 20ms 防呆上限 */
        {
            break;
        }
    }
    return (uint32_t)(DWT->CYCCNT - t0);
}

static void wire_probe(void)
{
    GPIO_InitTypeDef g = {0};
    uint32_t c_rs;
    uint32_t c_mosi;
    uint32_t c_miso;

    spi_wait_idle();
    c_rs = charge_cyc(BRD_SCR_RS_PORT, BRD_SCR_RS_PIN, (uint32_t)BRD_SCR_RS_PIN);
    c_mosi = charge_cyc(BRD_SCR_SPI_MOSI_PORT, BRD_SCR_SPI_MOSI_PIN, (uint32_t)BRD_SCR_SPI_MOSI_PIN);
    c_miso = charge_cyc(BRD_SCR_SPI_MISO_PORT, BRD_SCR_SPI_MISO_PIN, (uint32_t)BRD_SCR_SPI_MISO_PIN);

    LOG_I("disp", "wireprobe cyc: RS(PB9)=%lu MOSI(PA7)=%lu MISO(PA6)=%lu", (unsigned long)c_rs,
          (unsigned long)c_mosi, (unsigned long)c_miso);
    LOG_I("disp", "wireprobe: RS close to MOSI => RS wire reaches the module | RS << MOSI => RS OPEN");

    /* 还原：MOSI/MISO 回 AF5(SPI1)，CS/RS 回推挽输出并选回 LCD */
    g.Mode = GPIO_MODE_AF_PP;
    g.Pull = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    g.Alternate = GPIO_AF5_SPI1;
    g.Pin = BRD_SCR_SPI_MOSI_PIN;
    HAL_GPIO_Init(BRD_SCR_SPI_MOSI_PORT, &g);
    g.Pull = GPIO_PULLUP;
    g.Pin = BRD_SCR_SPI_MISO_PIN;
    HAL_GPIO_Init(BRD_SCR_SPI_MISO_PORT, &g);

    g.Mode = GPIO_MODE_OUTPUT_PP;
    g.Pull = GPIO_NOPULL;
    g.Pin = (uint32_t)(BRD_SCR_CS_PIN | BRD_SCR_RS_PIN);
    HAL_GPIO_Init(BRD_SCR_CS_PORT, &g);
    DISP_CS_SEL_LCD();
    DISP_RS_CMD();
    __HAL_SPI_ENABLE(&s_spi); /* 切脚期间 SPI 外设没被禁，重使能一次兜底 */
}

/* ★ RS 线归属体检（"背光跟随法"）：把 RS 拉低 1.5s，再拉高 1.5s，看屏有没有变暗。
 * 为什么需要它：白屏排查到最后只剩 RS 一根线（字库能证 SCK/MOSI/MISO/CS，唯独证不了 RS）。
 * 而 RS 线接错脚时，最常见的落点就是它隔壁的 **BLK（背光）** —— 背光高有效、是屏上最亮的负载，
 * 它跟着 RS 走，肉眼绝对看得出来：
 *   · 拉低这 1.5s 里屏**明显变暗/关灭** → 我们的 RS 线接在**模块的 BLK 脚**上（接错脚！），
 *     于是模块的 RS 输入悬空 → 面板收不到任何命令、像素全进不去 → 正是"整片白、永不变化"；
 *   · 屏毫无变化 → RS 线不在 BLK 上（在 RS 脚上但没生效，或根本没接上）。
 * 这一步不需要面板配合，是纯"输出脚 → 背光"的因果，能一次把"接错脚"这一类彻底排除或坐实。 */
static void rs_follow_check(void)
{
    uint32_t k;

    /* ★ 做成"**可数**"的：4 次短暗（RS 低 300ms / 高 300ms）。
     * 为什么改：单次长暗（1s）用户只能回"好像暗了一下"——**它和初始化那 240ms 的暗分不开**
     * （初始化是一长串命令 = RS 全程低）。改成 4 次规律短暗后，"屏是不是**规律地暗了 4 下**"
     * 就是能把假设一刀切开的答案：
     *   · 规律暗 4 下 → 我们的 RS 线在驱动**背光** → 它接在模块的 BLK 脚上；
     *   · 只有启动那一下、后面不规律 → RS 不控制背光 → 线没落在 BLK 上（落在 RS 脚但没生效 / 断了）。 */
    for (k = 0u; k < 4u; k++)
    {
        DISP_RS_CMD(); /* PB9 = 0 */
        disp_delay_us(300000u);
        DISP_RS_DATA(); /* PB9 = 1 */
        disp_delay_us(300000u);
    }
    LOG_I("disp", "rsfollow: 4 dim-blinks done (RS LOW/HIGH x4, 300ms each)");
    LOG_I("disp", "rsfollow: 4 dim blinks => RS wire is on the module BLK pin | not 4 => RS not on BLK");
}

/* ============================ 开机彩屏体检（交付形态） ============================
 * 不做图案自检：上电把整屏依次填 红 → 绿 → 蓝 → 白 → 黑，每帧停留 DISP_ST7735S_COLTEST_MS，
 * 每帧一行日志（帧序+颜色名）。为什么这种比图案自检好用：**不需要"知道该长什么样"**，
 * 只看两件事 —— ① 颜色按 红/绿/蓝/白/黑 顺序整屏变换（顺序对 = 通道与字节序对）
 * ② 每帧**整屏均匀**、四边没有杂色条（有条 = 坐标偏移，可见区没被写满 → 调 XSTART/YSTART）。
 * 最后停在纯黑：既给随后上屏的 Monitor 界面留干净底板，也顺手证明"屏上没有残留杂色"。
 * 想跳过整个体检：把 DISP_ST7735S_COLTEST_MS 改 0u（开机少 0.8s，但也就没有"面板活着"的直接证据）。 */
typedef struct
{
    uint16_t bg;
    uint16_t fg;
    const char *name;
    uint8_t label;
} Disp_ColorStep_t;

static const Disp_ColorStep_t s_coltest[] = {
    { 0xF800u, 0x0000u, "RED",   1u },
    { 0x07E0u, 0x0000u, "GREEN", 1u },
    { 0x001Fu, 0xFFFFu, "BLUE",  1u },
    { 0xFFFFu, 0x0000u, "WHITE", 1u },
    { 0x0000u, 0xFFFFu, "BLACK", 0u }, /* 收尾帧：不打标签，留干净黑底 */
};

static void panel_color_check(void)
{
    uint32_t n = (uint32_t)(sizeof(s_coltest) / sizeof(s_coltest[0]));
    uint32_t i;

    for (i = 0u; i < n; i++)
    {
        fb_fill(0, 0, (int16_t)DISP_ST7735S_W, (int16_t)DISP_ST7735S_H, s_coltest[i].bg);
        if (s_coltest[i].label != 0u)
        {
            fb_str(4, 4, s_coltest[i].name, s_coltest[i].fg, s_coltest[i].bg, DISP_FONT_MED);
        }
        /* ★ 凡改显存必须标脏（本函数用内部 fb_*，不经过 Disp_* 的自动标脏）——
         *   漏了它 = 这一帧根本没上屏（v0.1.69 的"花屏 + 空等"根因就是这个）。 */
        Disp_GeomInvalidateAll(&s_dirty, (int16_t)DISP_ST7735S_W, (int16_t)DISP_ST7735S_H);
        flush_dirty_now();

        LOG_I("disp", "coltest %lu/%lu %s", (unsigned long)(i + 1u), (unsigned long)n,
              s_coltest[i].name);
        if ((i + 1u) < n)
        {
            disp_delay_us((uint32_t)DISP_ST7735S_COLTEST_MS * 1000u);
        }
    }
    LOG_I("disp", "coltest done (order RED,GREEN,BLUE,WHITE,BLACK); edge stripe = XSTART/YSTART");
}

/* ============================ 契约实现 ============================ */

void Disp_Init(void)
{
    if (s_inited != 0u)
    {
        return;
    }
    disp_dwt_init();     /* DWT 计时（后续所有延时/超时都靠它，不依赖中断） */
    panel_pins_init();   /* 引脚：CS 先拉低（选中 LCD）再配输出；PB3=JTDO 配 AF5 */
    spi1_init();         /* SPI1 参数 + 使能 */
    s_inited = 1u;
    /* ★ w/h 先报 0 = "屏此刻不可用"（契约里的降级约定）。
     *   为什么必须这样（2026-09-19 实拍空屏的根因）：页面层有"内容没变就不重绘"的缓存，
     *   而 Monitor 任务比显示任务先跑（100ms 就刷屏，屏初始化要 1.6s）——若此时 w/h 报 128/160，
     *   页面层会正常走完绘制流程、把**缓存填满**，但驱动的绘制按 s_ready 被空操作掉；
     *   等屏就绪后内容是静态的 → 页面层判"没变化" → **永远不再画** → 屏停在纯黑。
     *   报 0 后页面层整体空操作（连缓存都不动），就绪后它的第一帧就是 force 全刷 → 一次点亮。 */
    s_info.w = 0u;
    s_info.h = 0u;
    /* ★ 这里**不做任何延时**：屏的上电稳定/软复位/唤醒/自检全部搬到 Disp_BringUp()，
     *   由显示任务在调度器启动后执行（原因见 disp_port.h 的说明与本文件顶部教训）。 */
}

void Disp_BringUp(void)
{
    if ((s_inited == 0u) || (s_bringup_done != 0u))
    {
        return;
    }
    s_bringup_done = 1u;

    if (s_spi_fail != 0u)
    {
        s_info.w = 0u; /* SPI 都没起来 → 页面层降级为空操作（屏坏了系统照跑） */
        s_info.h = 0u;
        LOG_E("disp", "SPI init failed -> display disabled (draw calls become no-ops)");
        return;
    }

    DISP_CS_SEL_LCD(); /* 事务起点：确保此刻选中的是 LCD 而不是字库 */
    disp_delay_us(120000u); /* 面板上电稳定 ≥120ms（硬件要求；任务里长等无妨） */
    panel_init_seq();       /* SWRESET/SLPOUT + 寄存器表 + DISPON（内含 2×120ms） */
    font_probe();           /* 字库芯片探测（顺带证总线） */

    /* ★ 整块 GRAM 打底写黑（**每次开机都做**，不是点灯期专用）：
     *   面板没被我们写过的像素显示的是**上电默认白**，而不同批次的可见区起点可能差 1~2 像素
     *   → 不打底就会在边上留一条白线（本项目实测：底部一条白线）。打底后任何未写区域都是黑，
     *   偏移误差被边框/黑色吃掉，肉眼不可见。代价 ≈45ms（132x162x2 字节 @10.5MHz+脉冲开销）。 */
    ggram_fill(0x0000u);

#if (DISP_ST7735S_DIAG_SEARCH > 0u)
    /* 点灯期搜索：一次烧录试完剩余全部假设（电气判据 + 偏移标定扫描；CS 风格已定案=每字节脉冲） */
    wire_probe();
    net_probe();
    pin_selftest();
    offset_sweep();
#elif (DISP_ST7735S_BUSCHECK_MS > 0u)
    wire_probe();
    net_probe();
    rs_follow_check();
    pin_selftest();
    panel_read_probe();
    bus_check();
#endif

#if (DISP_ST7735S_COLTEST_MS > 0u)
    panel_color_check(); /* 开机彩屏体检：红/绿/蓝/白/黑 整屏轮换，收尾纯黑 */
#endif

    s_info.w = DISP_ST7735S_W; /* 屏可用：把真实尺寸报给页面层（它下一帧就会全刷一次） */
    s_info.h = DISP_ST7735S_H;
    s_ready = 1u; /* ← 到这里屏才"可用"：之后页面层的绘制才开始真正上屏 */

    LOG_I("disp", "%s %ux%u selftest=%u flush=%luus pushes=%lu font=%u spifail=%u (bringup task)",
          s_info.name, (unsigned)s_info.w, (unsigned)s_info.h, (unsigned)Disp_SelfTest(),
          (unsigned long)s_last_flush_us, (unsigned long)s_push_cnt, (unsigned)s_font_ok,
          (unsigned)s_spi_fail);
}

const Disp_Info_t *Disp_Info(void)
{
    return &s_info;
}

void Disp_Clear(uint16_t color)
{
    if (s_ready == 0u)
    {
        return; /* 屏还没完成上电/初始化（显示任务在做）→ 本层空操作 */
    }
    fb_fill(0, 0, (int16_t)DISP_ST7735S_W, (int16_t)DISP_ST7735S_H, color);
    Disp_GeomInvalidateAll(&s_dirty, (int16_t)DISP_ST7735S_W, (int16_t)DISP_ST7735S_H);
}

void Disp_FillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color)
{
    int16_t cx = x;
    int16_t cy = y;
    int16_t cw = w;
    int16_t ch = h;

    if (s_ready == 0u)
    {
        return; /* 屏还没完成上电/初始化（显示任务在做）→ 本层空操作 */
    }
    if (Disp_GeomClip(&cx, &cy, &cw, &ch, (int16_t)DISP_ST7735S_W, (int16_t)DISP_ST7735S_H) == 0u)
    {
        return;
    }
    fb_fill(cx, cy, cw, ch, color);
    Disp_GeomAdd(&s_dirty, cx, cy, cw, ch, (int16_t)DISP_ST7735S_W, (int16_t)DISP_ST7735S_H,
                 DISP_DIRTY_FULL_RATIO);
}

void Disp_Text(int16_t x, int16_t y, const char *s, uint16_t fg, uint16_t bg, Disp_Font_e sz)
{
    uint8_t w = (sz == DISP_FONT_SMALL) ? 6u : 8u;
    uint8_t h = (sz == DISP_FONT_SMALL) ? 8u : 16u;
    uint16_t n;

    if ((s_ready == 0u) || (s == NULL))
    {
        return;
    }
    fb_str(x, y, s, fg, bg, sz);

    n = (uint16_t)0u;
    while (s[n] != '\0')
    {
        n++;
    }
    Disp_GeomAdd(&s_dirty, x, y, (int16_t)(n * w), (int16_t)h, (int16_t)DISP_ST7735S_W,
                 (int16_t)DISP_ST7735S_H, DISP_DIRTY_FULL_RATIO);
}

void Disp_TextCn(int16_t x, int16_t y, const char *gb2312, uint16_t fg, uint16_t bg, Disp_Font_e sz)
{
    /* has_cn=0（S4 落地：读板载字库 ROM + 缓存，见编码方案 §6）。
     * 现在静默不画 —— 页面层据 Disp_Info()->has_cn 回落英文标签，绝不会出现半截乱码。 */
    (void)x;
    (void)y;
    (void)gb2312;
    (void)fg;
    (void)bg;
    (void)sz;
}

void Disp_Line(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color)
{
    int16_t ax = (x0 < x1) ? x0 : x1;
    int16_t ay = (y0 < y1) ? y0 : y1;
    int16_t bx = (x0 < x1) ? x1 : x0;
    int16_t by = (y0 < y1) ? y1 : y0;

    if (s_ready == 0u)
    {
        return; /* 屏还没完成上电/初始化（显示任务在做）→ 本层空操作 */
    }
    fb_line(x0, y0, x1, y1, color);
    Disp_GeomAdd(&s_dirty, ax, ay, (int16_t)(bx - ax + 1), (int16_t)(by - ay + 1),
                 (int16_t)DISP_ST7735S_W, (int16_t)DISP_ST7735S_H, DISP_DIRTY_FULL_RATIO);
}

void Disp_Rect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color, uint8_t filled)
{
    int16_t cx = x;
    int16_t cy = y;
    int16_t cw = w;
    int16_t ch = h;

    if (s_ready == 0u)
    {
        return; /* 屏还没完成上电/初始化（显示任务在做）→ 本层空操作 */
    }
    if (Disp_GeomClip(&cx, &cy, &cw, &ch, (int16_t)DISP_ST7735S_W, (int16_t)DISP_ST7735S_H) == 0u)
    {
        return;
    }
    if (filled != 0u)
    {
        fb_fill(cx, cy, cw, ch, color);
    }
    else
    {
        fb_line(cx, cy, (int16_t)(cx + cw - 1), cy, color);
        fb_line(cx, (int16_t)(cy + ch - 1), (int16_t)(cx + cw - 1), (int16_t)(cy + ch - 1), color);
        fb_line(cx, cy, cx, (int16_t)(cy + ch - 1), color);
        fb_line((int16_t)(cx + cw - 1), cy, (int16_t)(cx + cw - 1), (int16_t)(cy + ch - 1), color);
    }
    Disp_GeomAdd(&s_dirty, cx, cy, cw, ch, (int16_t)DISP_ST7735S_W, (int16_t)DISP_ST7735S_H,
                 DISP_DIRTY_FULL_RATIO);
}

void Disp_Flush(void)
{
    if (s_ready == 0u)
    {
        return; /* 屏还没完成上电/初始化（显示任务在做）→ 本层空操作 */
    }
    flush_dirty_now(); /* 无脏区时立即返回（不产生 SPI 流量） */

    s_last_flush_ms = HAL_GetTick(); /* 自检心跳（调度器起来后才有意义） */
    s_flush_seen = 1u;
}

void Disp_Brightness(uint8_t pct)
{
    (void)pct; /* has_brightness=0（BLK 接 3V3） */
}

uint32_t Disp_LastFlushUs(void)
{
    return s_last_flush_us;
}

/* 推屏次数：flush= 数值冻住时用它区分"画面没变化（正常）"与"推屏停了（故障）" */
uint32_t Disp_PushCount(void)
{
    return s_push_cnt;
}

uint8_t Disp_SelfTest(void)
{
    if ((s_ready == 0u) || (s_info.w == 0u))
    {
        return 3u; /* FAIL：没初始化 / SPI 初始化失败（0 是 #ST 的"未编译"，驱动不许返回 0） */
    }
    if (s_spi_fail != 0u)
    {
        return 3u; /* FAIL：SPI 传输超时（外设/时钟/接线） */
    }
    if (s_push_cnt == 0u)
    {
        return 3u; /* FAIL：一次都没推过屏（自检画面都没上屏 = 屏上什么都没有） */
    }
    if ((s_flush_seen != 0u) && ((uint32_t)(HAL_GetTick() - s_last_flush_ms) >= 2000u))
    {
        return 2u; /* WARN：2s 没推屏 → 屏或监视任务停了 */
    }
    if (s_font_ok == 0u)
    {
        return 2u; /* WARN：字库芯片没读到（S4 汉字能力存疑；显示本身不受影响） */
    }
    return 1u;
}

#endif /* FEATURE_DISP_ST7735S_SPI */
