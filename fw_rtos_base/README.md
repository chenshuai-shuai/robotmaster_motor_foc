# fw_rtos_base 工程说明

**当前用途（2026-09）**：这块 RoboMaster A 板（F427IIH6）是 **8108 关节电机的 CAN 控制器**——
上位机用串口 ASCII 命令（`#V 10`）下发目标，A 板跑 200Hz 外环，通过 CAN 的 MIT 帧（0x201）控制电机；
反馈帧（0x781）回来后上 OLED、上日志、上报遥测 `@TEL`。

## 📚 代码文档（按顺序读就能看懂工程怎么跑）

| 文档 | 内容 |
|---|---|
| `docs/code/00_总览与阅读指南.md` | **总纲**：架构框图、任务表、启动时序、四条数据通路、术语表、踩坑表 |
| `docs/code/流程图_交互版.html` | **交互式流程图**（浏览器打开）：点模块看职责/真实函数/参数/坑，可搜索任意函数 |
| `docs/code/01_启动与任务框架.md` | main.c / FreeRTOS / 中断 / 版本号 / 崩溃黑匣子 |
| `docs/code/02_CAN底层与8108驱动.md` | CAN 收发、滤波器、MIT 帧打包解包、双编口径 |
| `docs/code/03_控制核算法.md` | 控制核 FSM、PI/PD+前馈、限幅、保护、事件（含公式与算例） |
| `docs/code/04_串口协议链.md` | ISR→环形缓冲→拼行→解析→分派→回包→日志 |
| `docs/code/05_控制任务与帧生成.md` | J8108 任务 200Hz 帧发生器、握手、看门狗、恢复 |
| `docs/code/06_显示按键与监控.md` | Monitor 任务、OLED 四页、按键事件机、UI 策略 |
| `docs/code/07_CAN运动控制学习指南.md` | **教学**：怎么让电机转得好、实验手册、自研固件路线图 |
| `docs/code/_函数地图.md` | 全工程 103 文件 / 383 个函数（按行号）清单 |
| `docs/操作手册_串口命令.md` | 发什么命令 → 收什么回包 → 电机做什么 |
| `docs/测试手册_8108串口控制.md` | 每次改动后的验收清单（TM-n，含逐字预期） |

一键核验 / 台架冒烟（在 `四轮麦轮小车/` 根目录）：
`PYTHONPATH= /c/msys64/usr/bin/make verify` · `make fast` · `make build` · `make bench PORT=COM3`

> 文档体检：`python fw_rtos_base/tools/check_code_docs.py`（检查文档有没有编造函数名/漏写函数）

## 目录结构

```
fw_rtos_base/
├── F427IIH6_CAN.ioc        ← CubeMX 工程（改配置从这里）
├── Core/                   ← 生成代码（HAL 初始化、main、中断）
├── Task/                   ← ★ 我们自己的任务（J8108_Task、CmdRx_Task、Monitor_Task、Oled_Task、Led_Task）
├── mcu_bsp/                ← 板级驱动（ctrl 控制核 / proto 协议 / Motor 8108 / can / uart / oled / key / log / sys_status）
├── docs/                   ← ★ 文档（code/ 为代码详解与流程图）
├── tools/                  ← verify_dev.py 核验 · serial_bench.py 台架 · check_code_docs.py 文档体检 · gen_flow_html.py
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
