#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
check_code_docs.py — 代码详解文档的「真实性 + 覆盖率」验收器

背景：docs/code/*.md 由多人（含子代理）分册撰写，最大风险是
  ① 编造不存在的函数/宏（幻觉）
  ② 漏写负责范围内的函数

做法：
  A. 从 docs/code/_函数地图.md 解析出「全工程真实函数集」（文件 → 函数名集合）
  B. 从每篇文档中抽取所有 `` `Ident(` `` 形式的函数提及
  C. 分类：known（真实存在）/ lib（HAL/FreeRTOS/CubeMX/标准库白名单）/ unknown（可疑=可能编造）
  D. 覆盖率：按映射表把文档↔文件对应起来，报告每篇文档对其负责文件的函数覆盖率与遗漏清单

用法：
    python tools/check_code_docs.py            # 全部检查
    python tools/check_code_docs.py --strict   # unknown 非空则退出码 1（CI 用）
"""
import io
import os
import re
import sys

BASE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DOCS = os.path.join(BASE, "docs", "code")
MAP = os.path.join(DOCS, "_函数地图.md")

# 文档 ↔ 负责文件（与派工单一致）
COVERAGE = {
    "01_启动与任务框架.md": [
        "Core/Src/main.c", "Core/Src/freertos.c", "Core/Src/stm32f4xx_it.c",
        "Core/Src/can.c", "Core/Src/gpio.c", "Core/Src/dma.c", "Core/Src/tim.c", "Core/Src/usart.c",
        "Core/Inc/FreeRTOSConfig.h", "Core/Inc/main.h",
        "Task/Inc/stack_probe.h", "Task/Src/Led_Task.c", "Task/Inc/Led_Task.h",
        "mcu_bsp/sys_status/sys_status.c", "mcu_bsp/sys_status/sys_status.h",
        "mcu_bsp/sys_status/fault_log.c", "mcu_bsp/sys_status/fault_log.h",
    ],
    "02_CAN底层与8108驱动.md": [
        "mcu_bsp/can/bsp_can.c", "mcu_bsp/can/bsp_can.h",
        "mcu_bsp/Motor/motor_8108.c", "mcu_bsp/Motor/motor_8108.h",
    ],
    "03_控制核算法.md": ["mcu_bsp/ctrl/ctrl_core.c", "mcu_bsp/ctrl/ctrl_core.h"],
    "04_串口协议链.md": [
        "mcu_bsp/proto/cmd_parse.h", "mcu_bsp/proto/proto_tx.c", "mcu_bsp/proto/proto_tx.h",
        "Task/Src/CmdRx_Task.c", "Task/Inc/CmdRx_Task.h", "Task/Inc/ui_status.h",
        "mcu_bsp/uart/bsp_usart.c", "mcu_bsp/uart/bsp_usart.h", "mcu_bsp/uart/uart10_def.c",
        "mcu_bsp/log/bsp_log.c", "mcu_bsp/log/bsp_log.h",
    ],
    "05_控制任务与帧生成.md": ["Task/Src/J8108_Task.c", "Task/Inc/J8108_Task.h"],
    "06_显示按键与监控.md": [
        "Task/Src/Monitor_Task.c", "Task/Inc/Monitor_Task.h", "Task/Inc/ui_status.h",
        "Task/Src/Oled_Task.c", "Task/Inc/Oled_Task.h",
        "mcu_bsp/key/bsp_key.c", "mcu_bsp/key/bsp_key.h", "mcu_bsp/key/key_core.c", "mcu_bsp/key/key_core.h",
        "mcu_bsp/Motor/ui_action.h",
    ],
}

# 库函数/标准库/RTOS/CubeMX 白名单（前缀或精确名）
LIB_PREFIX = ("HAL_", "MX_", "LL_", "GPIO", "__", "os", "vTask", "xTask", "uxTask", "vPort", "xPort",
              "xSemaphore", "xEventGroup", "xQueue", "xTimer", "pd", "prv", "config", "taskENTER",
              "taskEXIT", "port", "assert", "printf", "snprintf", "sprintf", "sscanf", "memcpy", "memset",
              "memcmp", "strlen", "strcmp", "strncmp", "strcpy", "strncpy", "strstr", "strchr", "atoi",
              "atof", "isalpha", "isdigit", "isspace", "tolower", "toupper", "fabs", "sqrt", "sin", "cos",
              "atan2", "pow", "floor", "ceil", "round", "fmod", "log10", "exp", "OLED_", "HAL", "CMSIS")
LIB_EXACT = {
    "main", "loop", "if", "for", "while", "switch", "return", "sizeof", "case", "else", "do", "int", "float",
    "void", "char", "uint8_t", "uint16_t", "uint32_t", "int32_t", "bool", "static", "const", "struct",
    "define", "include", "ifdef", "endif", "ifndef", "func", "函数", "此处", "略",
}


def parse_map():
    """返回 {文件: {函数名: 行号}}"""
    out = {}
    cur = None
    for line in io.open(MAP, encoding="utf-8", errors="replace"):
        m = re.match(r"^##\s+(\S+\.(?:c|h))\s+\(", line)
        if m:
            cur = m.group(1)
            out[cur] = {}
            continue
        m2 = re.match(r"^\|\s*(\d+)\s*\|\s*`([A-Za-z_]\w*)`\s*\|", line)
        if m2 and cur:
            out[cur][m2.group(2)] = int(m2.group(1))
    return out


def collect_macros():
    """扫描源码收集所有 #define 名字（宏提及不算编造）"""
    names = set()
    for d in ("Core/Src", "Core/Inc", "Task/Src", "Task/Inc", "mcu_bsp", "Device"):
        root = os.path.join(BASE, d.replace("/", os.sep))
        for dp, _, fs in os.walk(root):
            if "Drivers" in dp or "Middlewares" in dp:
                continue
            for fn in fs:
                if fn.endswith((".c", ".h")):
                    txt = io.open(os.path.join(dp, fn), encoding="utf-8", errors="replace").read()
                    for m in re.finditer(r"^\s*#\s*define\s+([A-Za-z_]\w*)", txt, re.M):
                        names.add(m.group(1))
    return names


def collect_files():
    """工程内真实文件名（用于核查文档里提到的 xxx.c/xxx.h 是否存在）"""
    base = set()
    for d in ("Core/Src", "Core/Inc", "Task/Src", "Task/Inc", "mcu_bsp", "Device"):
        root = os.path.join(BASE, d.replace("/", os.sep))
        for dp, _, fs in os.walk(root):
            for fn in fs:
                base.add(fn)
    return base


def is_lib_file(fn, real_files):
    if fn in real_files:
        return True
    if any(r.endswith(fn) for r in real_files):   # hal_conf.h ⊂ stm32f4xx_hal_conf.h 之类的简写
        return True
    for h in ("math.h", "string.h", "stdio.h", "stdint.h", "stdbool.h", "stdlib.h"):
        if fn == h:
            return True
    # 已删除文件的历史提及（文档里作为"曾经存在/已删"说明用）
    for gone in ("j8108_action.h",):
        if fn == gone:
            return True
    # CubeMX/HAL/RTOS 标准文件名放行
    for p in ("stm32f4xx", "cmsis_", "tasks.c", "timers.c", "port.c", "queue.c", "list.c", "heap_",
              "FreeRTOS", "system_stm32", "startup_", "RTE_", "main.h", "main.c"):
        if fn.startswith(p):
            return True
    return False



def collect_lib_names():
    """扫描 Middlewares/Drivers（HAL/FreeRTOS/CMSIS）里的函数名与函数式宏，作为白名单来源"""
    names = set()
    for d in ("Middlewares", "Drivers"):
        root = os.path.join(BASE, d)
        if not os.path.isdir(root):
            continue
        for dp, _dirs, fs in os.walk(root):
            for fn in fs:
                if not fn.endswith((".c", ".h")):
                    continue
                try:
                    txt = io.open(os.path.join(dp, fn), encoding="utf-8", errors="replace", newline="").read()
                except Exception:
                    continue
                for line in txt.split("\n"):
                    q = line.find("(")
                    if q <= 0 or line.rstrip().endswith(";"):
                        continue
                    m = re.search(r"([A-Za-z_]\w*)\s*$", line[:q])
                    if m:
                        names.add(m.group(1))
                for m in re.finditer(r"^\s*#\s*define\s+([A-Za-z_]\w*)\s*\(", txt, re.M):
                    names.add(m.group(1))
    return names


def collect_lib_files():
    """Middlewares/Drivers 下的真实文件名（这些目录里的 .c/.h 提及不算编造）"""
    base = set()
    for d in ("Middlewares", "Drivers"):
        root = os.path.join(BASE, d)
        for dp, _dirs, fs in os.walk(root):
            for fn in fs:
                base.add(fn)
    return base


def looks_like_pin(name):
    """引脚/信号/杂项名（PG9、CAN1_RX、DMA2_Stream1、TIM4_CH1 …）不是函数名"""
    if re.fullmatch(r"P[A-H]\d{1,2}", name):
        return True
    if re.fullmatch(r"[A-Z][A-Z0-9]{1,7}_(RX|TX|CH\d|IRQn?|NVIC|CLK|EN|Stream\d|Channel\d)", name):
        return True
    if re.fullmatch(r"(CAN|USART|UART|TIM|SPI|I2C|ADC|DMA|SDIO|USB|GPIO|EXTI|RCC|NVIC)[0-9A-Z_]*", name):
        return True
    return name in ("FromISR", "CLOCK", "sgn", "vsnprintf", "snprintf", "sprintf", "memmove", "main_cpp", "module_callback")


def doc_files():
    if not os.path.isdir(DOCS):
        return []
    return sorted(f for f in os.listdir(DOCS) if f.endswith(".md") and not f.startswith("_"))


def mentions_strict(text):
    """严格：只把「当函数调用写」的形态算作函数提及（`Name(` 或 Name()）"""
    got = set()
    for m in re.finditer(r"`([A-Za-z_]\w{2,})\s*\(", text):
        got.add(m.group(1))
    for m in re.finditer(r"\b([A-Za-z_]\w{3,})\s*\(\)", text):
        got.add(m.group(1))
    return got


def mentions(text):
    """宽松：`Name(` / Name() / 反引号包裹的 `Name`（文档小标题常用）——用于覆盖率统计"""
    got = set()
    for m in re.finditer(r"`([A-Za-z_]\w{2,})\s*\(", text):   # `Name(  参数…`
        got.add(m.group(1))
    for m in re.finditer(r"\b([A-Za-z_]\w{3,})\s*\(\)", text):  # 裸写 Name()
        got.add(m.group(1))
    for m in re.finditer(r"`([A-Za-z_]\w{2,})`", text):        # `Name`（不带括号，文档小标题常用）
        got.add(m.group(1))
    # 斜杠简写展开：CAN2_RX0/RX1_IRQHandler → CAN2_RX0_IRQHandler + CAN2_RX1_IRQHandler
    for m in re.finditer(r"`?([A-Za-z_]\w*)/([A-Za-z_]\w*)`?", text):
        a, b = m.group(1), m.group(2)
        head_a = a[:a.rfind("_") + 1] if "_" in a else ""
        tail_b = b[b.find("_") + 1:] if "_" in b else b
        if head_a and tail_b:
            got.add(a + "_" + tail_b)      # CAN2_RX0 + IRQHandler
            got.add(head_a + b)            # CAN2_ + RX1_IRQHandler
    return got


def proto_keys():
    """从 CmdRx_Task.c 的 set_one 键表里抽取 #SET 键名（这些是协议键，不是函数）"""
    keys = set()
    try:
        txt = io.open(os.path.join(BASE, "Task", "Src", "CmdRx_Task.c"), encoding="utf-8",
                      errors="replace").read()
    except Exception:
        return keys
    for m in re.finditer(r'strcmp\s*\(\s*key\s*,\s*"([^"]+)"', txt):   # #SET 键表（set_one）
        keys.add(m.group(1))
    for m in re.finditer(r'set_one\s*\(\s*"([^"]+)"', txt):
        keys.add(m.group(1))
    return keys


def is_lib(name):
    if name in LIB_EXACT:
        return True
    for p in LIB_PREFIX:
        if p and name.startswith(p) and len(p) > 1:
            return True
    return False


def main():
    strict = "--strict" in sys.argv
    fmap = parse_map()
    all_funcs = {f for d in fmap.values() for f in d}
    docs = doc_files()
    if not docs:
        print("!! docs/code 下没有文档")
        return 1
    if not fmap:
        print("!! 未解析到 _函数地图.md（先运行生成脚本）")
        return 1

    total_bad = 0
    macros = collect_macros()
    real_files = collect_files() | collect_lib_files()
    lib_names = collect_lib_names()
    pkeys = proto_keys()
    all_names = all_funcs | macros | lib_names | pkeys
    print("=" * 78)
    print("代码详解文档验收：真实性（有无编造）+ 覆盖率（有无漏写）")
    print(f"真实函数集：{len(fmap)} 文件 / {len(all_funcs)} 唯一函数名 / {len(macros)} 宏")
    print("=" * 78)

    # ---- A. 真实性 ----
    print("\n【A. 真实性】文档提及但工程里不存在的函数名（疑似编造）")
    for d in docs:
        p = os.path.join(DOCS, d)
        text = io.open(p, encoding="utf-8", errors="replace").read()
        mentioned = mentions_strict(text)
        bad = []
        for n in mentioned:
            if n in all_names or is_lib(n) or looks_like_pin(n):
                continue
            if n.isupper() and "_" not in n and len(n) <= 8:
                continue            # 页面名/标签（LINK/MOTION/SPEED …）
            # 缩写放行：ReqStart ⊂ J8108_ReqStart（文档里常见的简写）
            if any(a.endswith(n) and len(a) >= len(n) + 2 for a in all_names):
                continue
            bad.append(n)
        bad.sort()
        tag = "OK " if not bad else "!! "
        print(f"  {tag}{d:34s} 提及 {len(mentioned):4d} 个函数  可疑 {len(bad)}")
        for n in bad[:25]:
            print(f"        · {n}")
        total_bad += len(bad)

    # ---- A2. 文件名真实性 ----
    print("\n【A2. 文件名】文档里提到的 xxx.c/xxx.h 是否真实存在")
    file_bad_total = 0
    for d in docs:
        text = io.open(os.path.join(DOCS, d), encoding="utf-8", errors="replace").read()
        got = set(re.findall(r"\b([A-Za-z0-9_]+\.(?:c|h))\b", text))
        badf = sorted(f for f in got if not is_lib_file(f, real_files))
        tag = "OK " if not badf else "!! "
        print(f"  {tag}{d:34s} 提及 {len(got):3d} 个文件名  可疑 {len(badf)}")
        for f in badf[:15]:
            print(f"        · {f}")
        file_bad_total += len(badf)

    # ---- 覆盖率 ----
    print("\n【B. 覆盖率】按派工范围核对（文档 ↔ 负责文件）")
    grand_miss = 0
    for d, files in COVERAGE.items():
        p = os.path.join(DOCS, d)
        if not os.path.exists(p):
            print(f"  !! {d:34s} 【缺失】文件不存在")
            grand_miss += 1
            continue
        text = io.open(p, encoding="utf-8", errors="replace").read()
        seen = mentions(text)
        want = {}
        for f in files:
            for fn in fmap.get(f, {}):
                want[fn] = f
        miss = sorted(n for n in want if n not in seen)
        cov = 100.0 * (len(want) - len(miss)) / max(1, len(want))
        print(f"  {'OK ' if not miss else '!! '}{d:34s} 覆盖 {len(want)-len(miss):3d}/{len(want):3d} ({cov:5.1f}%)  遗漏 {len(miss)}")
        if miss:
            # 按文件归组显示，便于定位
            byf = {}
            for n in miss:
                byf.setdefault(want[n], []).append(n)
            for f, ns in sorted(byf.items()):
                print(f"        · {f}: {', '.join(ns[:12])}{' …' if len(ns) > 12 else ''}")
        grand_miss += len(miss)

    print("\n" + "=" * 78)
    print(f"汇总：可疑函数名 {total_bad} 处；可疑文件名 {file_bad_total} 处；覆盖遗漏 {grand_miss} 处")
    print("提示：lib 白名单外的可疑名要人工确认；覆盖率低不等于错（有些文件是 CubeMX 模板/未用函数）")
    print("=" * 78)
    if strict and (total_bad or grand_miss or file_bad_total):
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
