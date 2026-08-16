# fw_rtos_base 工程说明

四轮麦轮小车 RTOS 基础工程。基于 328dragon/rm_a_board_f427IIH6 改造。

## 目录结构

```
fw_rtos_base/
├── F427IIH6_CAN.ioc        ← CubeMX 工程（改配置从这里）
├── Core/                   ← 生成代码（HAL 初始化、main、中断）
├── Task/                   ← ★ 我们自己的任务（Led_Task、MotorTest）
├── mcu_bsp/                ← 板级驱动（CAN/串口/电机/日志，模块化）
├── Device/                 ← 原作者舵机等（阶段1停用）
├── USB_DEVICE/             ← USB CDC 虚拟串口（2026-08-16 新增）
├── Middlewares/            ← FreeRTOS + USB 库
├── Drivers/                ← HAL/CMSIS
├── MDK-ARM/                ← Keil 工程（编译/烧录从这里）
└── fix_after_mx.py         ← ★ CubeMX 生成后必跑（见下）
```

## 开发流程（重要）

1. **改外设配置** → CubeMX 打开 `F427IIH6_CAN.ioc`（本机 CubeMX 实际 6.18.1）→ GENERATE CODE
2. **生成后必跑**：`python fix_after_mx.py`（修复 CubeMX 破坏的 4 处：GCC port/uvprojx/USB init/停用项）
3. **检查**：can.c 里 `hcan1` 存在（CubeMX 6.18 有时丢 CAN1 配置）、Keil Flash 算法在位
4. VSCode 编译烧录（Keil GUI 勿同时开）

## 关键配置

| 项 | 值 |
|---|---|
| 主频 | 168MHz（HSE 12MHz × PLL） |
| CAN1 | PD0/PD1，1Mbps（Prescaler=6, BS1=5TQ, BS2=1TQ, SJW=1），ABOM/NART 开 |
| CAN2 | PB12/PB13，1Mbps 同上 |
| USART6 日志 | PG9/PG14 + DMA |
| USB | OTG_FS Device_Only，CDC 类，48MHz 来自 PLLQ=7 |
| FreeRTOS | 静态 Idle + heap_4 动态（15KB），tick 1kHz |
| printf | fputc → USART6（重定向在 mcu_bsp/log/bsp_log.c） |

## 电机相关（阶段2）

- C610 电流换算：`cur = target_A / 10 * 10000`（-10000~10000 ↔ ±10A）
- M2006：36:1 减速、7 极对、0.18 N·m/A，堵转 27.3A 必须限流
- 测试任务：`Task/Src/MotorTest_Task.c`（开环斜坡状态机，main.c 里取消注释启用）
