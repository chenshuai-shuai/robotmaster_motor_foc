# 四轮麦轮小车 · 基础 RTOS 工程（fw_rtos_base）

> 基于开源工程 `328dragon/rm_a_board_f427IIH6` 改造（A板 + FreeRTOS + 模块化 CAN 驱动）
> 目标：**先跑通 1 台 M2006+C610，再扩展 4 电机全向移动**
> 知识库对应：`D:\知识库\wiki-a-board`（协议/参数/坑全在里面）

## 硬件清单

| 部件 | 型号 | 说明 |
|---|---|---|
| 主控 | RoboMaster 开发板A型（STM32F427IIH6） | CAN1=PD0/PD1，POWER1~4_CTRL=PH2~PH5 |
| 电机 | M2006 P36 ×4（先上 1 台） | 36:1 减速，1N·m 额定，4-Pin 传感器线 |
| 电调 | C610 ×4（先上 1 个） | 10A，**电流量程 -10000~10000 ↔ ±10A** |
| 电池 | 6S LiPo（22.2V，≤26V） | 建议串 20~30A 保险丝 |
| 日志 | 板载 USART（CubeMX 配 USART6，引脚按板子丝印） | 串口助手 波特率按 CubeMX 配置 |

## 接线（单电机）

```
6S电池 XT30 ──→ A板 电源输入（XT30）
A板 24V输出(接口#25) ──→ C610 电源线（XT30）
A板 CAN1(PD0/PD1, 接口#1) ──CAN线──→ C610 CAN口（A黑=CAN_L, B红=CAN_H）
C610 三相线 ──→ M2006 三相线（同色相接）
C610 4-Pin数据线 ──→ M2006 位置传感器口
```
⚠️ CAN 总线两端 120Ω 终端电阻拨 ON（A 板内部有终端，另一端 C610 拨 ON 前先确认——规范：总线两端接，中间不接）

## 编译步骤（重要）

1. **STM32CubeMX**（本机 6.8.0）打开 `F427IIH6_CAN.ioc`
2. 启用日志串口：**USART6**（Asynchronous，引脚选 A 板 UART 口对应引脚，参考板子背面丝印）
   - 或保持其他 USART，同步改 `mcu_bsp/uart/uart10_def.c` 里的 `huart6` 句柄
3. Generate Code（生成到本目录，覆盖同名文件；**生成后重放一遍下述用户代码改动**，或把用户文件放 Task/ 与 mcu_bsp/ 下不受影响）
4. 用 CubeIDE / MDK-ARM（工程在 `MDK-ARM/`）编译烧录
5. 打开串口助手 → 复位 → 看到 `[MotorTest]` 日志即成功

> 原工程任务入口在 `main_cpp()`（main.c USER CODE 2 调用）：已加入 `UART10_Init()` + `MotorTest_Task_Init()`。原舵机代码保留但未使能 PWM，不影响。

## 测试流程（自动状态机，无需遥控）

```
IDLE(2s) → RAMP_UP(电流0→0.8A, 5s) → HOLD(0.5A, 3s) → RAMP_DOWN(→0, 3s) → DONE → 循环
```
- 电机应：静止 → 缓慢加速转动 → 匀速 → 减速停止 → 重复
- 串口 1Hz 打印：相位、转子角度(0~8191→°)、转速(RPM)、电流原始值、命令电流
- **第一次上电**：如果电机不动，先检查 C610 ID（绿灯闪 N 次=ID N），SET 键设成 1（按 1 次）
- 转向反了：`motor_instnce[0].motor_cofig.motor_reverse_flag = MOTOR_DIRECTION_REVERSE`

## 关键代码位置

| 文件 | 内容 |
|---|---|
| `Task/Src/MotorTest_Task.c` | ⭐ 单电机测试任务（状态机/电流命令/反馈打印） |
| `mcu_bsp/Motor/c610.h` | C610 适配：电流换算宏（10000↔10A），复用 C620 驱动 |
| `mcu_bsp/Motor/c620.c` | 电机组驱动：注册(CANRegister)/反馈解析/电流打包发送 |
| `mcu_bsp/can/bsp_can.c` | 模块化 CAN 驱动（过滤器/中断/FIFO） |
| `mcu_bsp/uart/uart10_def.c` | 日志串口实例定义（原工程缺失，已补） |
| `Core/Src/maincpp.cpp` | 任务入口 main_cpp() |

## 已知坑（知识库认证）

1. **C610 电流量程 10000，不是 C620 的 16384**——网上 C620 教程直接抄会超量程
2. C610 反馈**无温度字段**（DATA[6]=Null）
3. 反馈 DATA[4:5] 手册标"实际输出转矩"，**未给换算尺度**——打印原始值先观察规律
4. `motor_instnce[].get.*` 里是 **CAN 原始值**（驱动未换算），使用前按协议换算
5. 未使能电机（motor_enable_flag=MOTOR_STOP）电流自动清零——只使能你要转的那台
6. 电池满电 25.2V 接近 A 板 26V 上限，稳压源先设好电压再接

## 下一步路线

1. ✅ 单电机开环转动（本工程）
2. 速度闭环（PID 速度环，pid.c 已有，可参考知识库 zhangpanyang 例程）
3. 4 电机注册（C610_Group_Register 组0=0x200 管 ID1-4，四台都接）
4. 麦轮运动学（4 轮转速 = f(前进/横移/旋转)）→ 全向移动

## USB CDC 日志口（2026-08-16 新增）

板载 USB 口（J14：DP=PA12/DM=PA11/ID=PA10，22Ω 串联 + ESD，VBUS 无检测脚）已配置为 **USB CDC 虚拟串口**：
- 一根 USB 线直连电脑，Win10+ 免驱，设备管理器出现 `USB 串行设备 (COMx)`
- 日志仍走 printf → 串口（USART6 PG9/PG14）与 USB 并存，后续可把日志切到 CDC
- 主频 **168MHz**（原 180，给 USB 48MHz 让路）；CAN 波特率已重算保持 1Mbps

## CubeMX 生成后必跑修复脚本 ⚠️

CubeMX（本机实际版本 6.18.1，目录名 stm32cubemx6.8.0 是旧的）每次 GENERATE CODE 会破坏 4 处，
生成后必须执行：

```
cd fw_rtos_base && python fix_after_mx.py
```

脚本自动修复：FreeRTOS GCC port（AC6 必需）、uvprojx 引用、main.c 的 USB 初始化与停用项。
已知 CubeMX 6.18 打开 6.14 格式 .ioc 会丢 CAN1 配置（PD0/PD1），生成后务必检查 can.c 里 hcan1 是否存在。

## 烧录环境（2026-08-14 修复记录）

- Keil MDK 精简版：`E:\keil_arm\AppData\Local\Keil_v5\UV4`（缺 STLink DLL/DFP pack，已手动补全）
- Flash 算法：Keil GUI 配 `STM32F4xx 2MB Flash` 后 **Ctrl+S 保存**（被 CubeMX 覆盖时需重配）
- 烧录失败先查：Keil GUI 是否开着（占 ST-Link）→ USB 线是否是数据线 → ST-LINK_CLI 连接测试
