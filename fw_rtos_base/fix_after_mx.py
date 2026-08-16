#!/usr/bin/env python
"""
fix_after_mx.py — CubeMX 重新生成代码后的自动修复脚本
用法: python fix_after_mx.py

CubeMX 每次 GENERATE CODE 都会破坏以下 4 处，此脚本一键修复：
  1. FreeRTOS port 被重置为 RVDS 版（AC5 专用，AC6 编译报 __forceinline 错误）
     → 从 Cube FW 库复制 GCC/ARM_CM4F port 回工程
  2. uvprojx 的 port.c 引用 / IncludePath 被改回 RVDS
     → 改为 GCC/ARM_CM4F
  3. main.c 里 MX_USB_DEVICE_Init() 调用丢失
     → 补回初始化区（MX_CAN1_Init 之后）
  4. main.c 里 main_cpp() 被还原（阶段1停用的舵机/遥控）
     → 重新注释
另外提醒检查：Keil Flash 算法（Utilities）——被覆盖时在 Keil GUI 重配。
"""
import os, re, shutil, sys

BASE = os.path.dirname(os.path.abspath(__file__))
FW_GCC_PORT = r"C:/Users/Administrator/STM32Cube/Repository/STM32Cube_FW_F4_V1.27.1/Middlewares/Third_Party/FreeRTOS/Source/portable/GCC/ARM_CM4F"

def fix_gcc_port():
    dst = os.path.join(BASE, "Middlewares", "Third_Party", "FreeRTOS", "Source", "portable", "GCC", "ARM_CM4F")
    os.makedirs(dst, exist_ok=True)
    for f in ("port.c", "portmacro.h"):
        src = os.path.join(FW_GCC_PORT, f)
        if not os.path.exists(src):
            print(f"  ⚠️ Cube 库缺 {f}: {src}")
            continue
        shutil.copy2(src, os.path.join(dst, f))
    print("  ✅ GCC port 复制回工程")

def fix_uvprojx():
    uv = os.path.join(BASE, "MDK-ARM", "F427IIH6_CAN.uvprojx")
    t = open(uv, encoding="utf-8", errors="ignore").read()
    n = t.count("portable/RVDS/ARM_CM4F")
    t = t.replace("portable/RVDS/ARM_CM4F", "portable/GCC/ARM_CM4F")
    open(uv, "w", encoding="utf-8", newline="").write(t)
    print(f"  ✅ uvprojx RVDS→GCC（{n} 处）")
    if "STM32F4xx_2048.FLM" not in t:
        print("  ⚠️ Flash 算法缺失！需 Keil GUI: Utilities→Add→STM32F4xx 2MB Flash→Ctrl+S")
    else:
        print("  ✅ Flash 算法在位")

def fix_main():
    p = os.path.join(BASE, "Core", "Src", "main.c")
    t = open(p, encoding="utf-8", errors="ignore").read()
    changed = 0
    # USB 初始化调用
    if "MX_USB_DEVICE_Init();" not in t:
        old = "  MX_CAN1_Init();\n  /* USER CODE BEGIN 2 */"
        if old in t:
            t = t.replace(old, "  MX_CAN1_Init();\n  MX_USB_DEVICE_Init();\n  /* USER CODE BEGIN 2 */", 1)
            changed += 1
    # main_cpp 停用
    m = re.search(r'\n\tmain_cpp\(\);', t)
    if m and "// main_cpp();" not in t:
        t = t.replace("\n\tmain_cpp();", "\n\t// main_cpp();  /* 阶段1停用（fix_after_mx 自动注释） */", 1)
        changed += 1
    if changed:
        open(p, "w", encoding="utf-8", newline="").write(t)
    print(f"  ✅ main.c 修复（{changed} 处）" if changed else "  ✅ main.c 无需修复")

if __name__ == "__main__":
    print("fix_after_mx: CubeMX 生成后自动修复")
    fix_gcc_port()
    fix_uvprojx()
    fix_main()
    print("完成！建议再跑一次编译确认 0 Error。")
