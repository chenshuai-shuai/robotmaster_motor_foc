#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
tools/check_profiles.py - 预设组合编译矩阵（把"宏接口"从文档变成可执行的检查）

用法:
    python tools/check_profiles.py --fast              # gcc -fsyntax-only 扫全部组合（秒级，不需要 Keil）
    python tools/check_profiles.py --real              # 临时 uvprojx + UV4 真编全部组合（每个约 1 分钟）
    python tools/check_profiles.py --real --only MINIMAL   # 只做某个组合（名字可省略 PROFILE_ 前缀）

原理:
    feature_config.h 里 CFG_PROFILE 用 #ifndef 包住 → 命令行 -DCFG_PROFILE=... 即可覆盖，
    因此"组合"不需要改任何文件。
    Keil 的 UV4 CLI 不支持 -D，所以 --real 模式**复制**一份 uvprojx，把 <Define> 补上
    CFG_PROFILE，并把输出目录/输出名改成组合专属（避免覆盖正式 hex），编完删除临时工程
    （try/finally 保证不留残留，正式工程的 hex/axf 永不被覆盖）。

为什么必须做这件事:
    没有组合编译，"宏接口"就只是文档 —— 迟早会烧一个"以为开了其实没开"的组合去台架。
    见 docs/1_规则（既定事实）/规范_功能宏与模块化.md（门禁 V13）。
"""
import argparse
import os
import re
import shutil
import subprocess
import sys

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

BASE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MDK = os.path.join(BASE, "MDK-ARM")
PROJ = "F427IIH6_CAN"
UV4 = r"E:\keil_arm\AppData\Local\Keil_v5\UV4\UV4.exe"
GCC = r"C:\msys64\mingw64\bin\gcc.exe"

PROFILES = ["PROFILE_FULL", "PROFILE_DISP_DEV", "PROFILE_MOTOR_DEV", "PROFILE_MINIMAL"]

INCLUDES = [
    "Core/Inc", "Drivers/STM32F4xx_HAL_Driver/Inc", "Drivers/STM32F4xx_HAL_Driver/Inc/Legacy",
    "Drivers/CMSIS/Device/ST/STM32F4xx/Include", "Drivers/CMSIS/Include",
    "Task/Inc", "Device/Inc",
    "mcu_bsp/can", "mcu_bsp/Motor", "mcu_bsp/ctrl", "mcu_bsp/proto", "mcu_bsp/key",
    "mcu_bsp/uart", "mcu_bsp/log", "mcu_bsp/oled", "mcu_bsp/sd", "mcu_bsp/fs",
    "mcu_bsp/version", "mcu_bsp/sys_status", "mcu_bsp/cli", "mcu_bsp/button",
    "mcu_bsp/protocol", "mcu_bsp/protocol_calibration",
    "Middlewares/Third_Party/FreeRTOS/Source/include",
    "Middlewares/Third_Party/FreeRTOS/Source/CMSIS_RTOS",
    "Middlewares/Third_Party/FreeRTOS/Source/portable/GCC/ARM_CM4F",
]

SRC_DIRS = ["Core/Src", "Task/Src", "mcu_bsp", "Device/Src"]

# V11 对象：契约头（任何组合都能单独 include 的头文件）
CONTRACT_HEADERS = [
    "Task/Inc/board_config.h", "Task/Inc/feature_config.h", "Task/Inc/ui_status.h",
    "Task/Inc/Oled_Task.h", "Task/Inc/Monitor_Task.h", "Task/Inc/J8108_Task.h",
    "Task/Inc/Led_Task.h", "Task/Inc/CmdRx_Task.h", "Task/Inc/SdCard_Task.h",
    "Task/Inc/stack_probe.h",
    "mcu_bsp/key/bsp_key.h", "mcu_bsp/proto/proto_tx.h", "mcu_bsp/oled/OLED.h",
    "mcu_bsp/Motor/motor_8108.h", "mcu_bsp/Motor/ui_action.h",
    "mcu_bsp/proto/cmd_parse.h", "mcu_bsp/log/bsp_log.h", "mcu_bsp/uart/bsp_usart.h",
]


def sources():
    """以 uvprojx 的实际编译集为准（只取工程自己的源码；HAL/CMSIS/FreeRTOS 不算宏矩阵范围）。

    为什么不用 os.walk 扫目录：目录里躺着不少**不在工程里**的历史文件
    （mcu_bsp/button/button.c、mcu_bsp/Math/tlsf.c …），它们不参与编译，
    拿它们当组合检查对象会给出假失败 —— 它们属于 M7 的清理清单。
    """
    proj = os.path.join(MDK, f"{PROJ}.uvprojx")
    out = []
    if os.path.exists(proj):
        t = open(proj, encoding="utf-8", errors="replace").read()
        for fp in re.findall(r"<FilePath>(.*?)</FilePath>", t):
            p = fp.replace("\\", "/").strip()
            if not p.endswith(".c"):
                continue
            rp = os.path.normpath(os.path.join(MDK, p))          # uvprojx 里是相对 MDK-ARM 的路径
            rel = os.path.relpath(rp, BASE).replace("\\", "/")
            if any(rel.startswith(d) for d in SRC_DIRS) and os.path.exists(rp):
                out.append(rel)
    if not out:  # 兜底：工程解析失败时退回目录扫描
        for d in SRC_DIRS:
            for dp, _dn, fn in os.walk(os.path.join(BASE, d.replace("/", os.sep))):
                for f in sorted(fn):
                    if f.endswith(".c"):
                        out.append(os.path.relpath(os.path.join(dp, f), BASE).replace("\\", "/"))
    return sorted(set(out))


def check_headers():
    """V11：契约头必须能**单独 include**（规范 R4）——不依赖包含顺序、不需要先 define 任何 FEATURE。

    做法：每个头生成一个临时 TU（只含那一行 include），用与 --fast 相同的 -I 做 -fsyntax-only，
    且**不带任何 FEATURE 定义**。M4 就是靠这类检查发现 `Led_Task.h` 少了 `<stdint.h>`。
    """
    if not os.path.exists(GCC):
        print(f"  SKIP  gcc 不存在：{GCC}")
        return 0
    print(f"---- --headers（V11）：{len(CONTRACT_HEADERS)} 个契约头单独 include ----")
    fails = []
    tmpd = os.path.join(BASE, "_hdr_check")
    os.makedirs(tmpd, exist_ok=True)
    try:
        for h in CONTRACT_HEADERS:
            tu = os.path.join(tmpd, "tu.c")
            with open(tu, "w", encoding="utf-8", newline="") as fh:
                fh.write(f'#include "{h}"\n')
            cmd = ([GCC, "-fsyntax-only", "-std=c99", "-DUSE_HAL_DRIVER", "-DSTM32F427xx", "-I."]
                   + [f"-I{os.path.relpath(i, BASE)}" for i in INCLUDES]
                   + [os.path.relpath(tu, BASE)])
            r = subprocess.run(cmd, cwd=BASE, capture_output=True, text=True,
                               encoding="utf-8", errors="replace")
            if r.returncode != 0:
                fails.append(h)
                msg = [l for l in (r.stderr or r.stdout).splitlines()
                       if ("error:" in l or "fatal error" in l)]
                print(f"  FAIL  {h}: {(msg[0].strip() if msg else 'syntax error')[:130]}")
            else:
                print(f"  PASS  {h}")
    finally:
        shutil.rmtree(tmpd, ignore_errors=True)
    return 1 if fails else 0


def check_fast(only=None):
    if not os.path.exists(GCC):
        print(f"  SKIP  gcc 不存在：{GCC}")
        return 0
    srcs = sources()
    print(f"---- --fast: gcc -fsyntax-only ×{len(srcs)} 个源文件 × {len(PROFILES)} 个组合 ----")
    fails = []
    for prof in PROFILES:
        if only and prof not in only:
            continue
        bad = []
        for s in srcs:
            cmd = [GCC, "-fsyntax-only", "-std=c99", "-DUSE_HAL_DRIVER", "-DSTM32F427xx",
                   f"-DCFG_PROFILE={prof}"]
            cmd += [f"-I{os.path.relpath(i, BASE)}" for i in INCLUDES]
            cmd.append(os.path.relpath(s, BASE))
            r = subprocess.run(cmd, cwd=BASE, capture_output=True, text=True,
                               encoding="utf-8", errors="replace")
            errs = [l for l in (r.stderr or "").splitlines() if ": error:" in l]
            if errs:
                bad.append((os.path.relpath(s, BASE), errs[0]))
        if bad:
            fails.append(prof)
            print(f"  FAIL  {prof}: {len(bad)} 个文件语法错误")
            for f, e in bad[:5]:
                print(f"          {f}: {e.strip()[:150]}")
        else:
            print(f"  PASS  {prof}")
    return 1 if fails else 0


def check_real(only=None):
    if not os.path.exists(UV4):
        print(f"  SKIP  UV4 不存在：{UV4}")
        return 0
    src_proj = os.path.join(MDK, f"{PROJ}.uvprojx")
    if not os.path.exists(src_proj):
        print(f"  SKIP  找不到工程：{src_proj}")
        return 0
    text0 = open(src_proj, encoding="utf-8", errors="replace").read()
    fails = []
    print(f"---- --real: 临时 uvprojx + UV4 构建 ×{len(PROFILES)} 个组合 ----")
    for prof in PROFILES:
        if only and prof not in only:
            continue
        tmp_proj = os.path.join(MDK, f"_profile_{prof}.uvprojx")
        out_dir = f"_profile_build\\{prof}\\"
        log = os.path.join(MDK, f"_profile_{prof}.log")
        # 补 CFG_PROFILE + 隔离输出目录/输出名（绝不碰正式 hex/axf）
        t = re.sub(r"<Define>(.*?)</Define>",
                   lambda m: f"<Define>{m.group(1)},CFG_PROFILE={prof}</Define>", text0, count=1)
        t = re.sub(r"<OutputDirectory>.*?</OutputDirectory>",
                   lambda m: f"<OutputDirectory>{out_dir}</OutputDirectory>", t, count=1)
        t = re.sub(r"<OutputName>.*?</OutputName>",
                   lambda m: f"<OutputName>{PROJ}_{prof}</OutputName>", t, count=1)
        os.makedirs(os.path.join(MDK, f"_profile_build", prof), exist_ok=True)
        try:
            open(tmp_proj, "w", encoding="utf-8", newline="").write(t)
            r = subprocess.run([UV4, "-b", os.path.basename(tmp_proj), "-j0", "-t", PROJ,
                                "-o", os.path.basename(log)], cwd=MDK,
                               capture_output=True, text=True, encoding="utf-8", errors="replace")
            txt = open(log, encoding="utf-8", errors="replace").read() if os.path.exists(log) else ""
            err_lines = [l.strip() for l in txt.splitlines() if re.search(r"\d+ Error\(s\)", l)]
            nerr = (re.search(r"(\d+) Error\(s\)", err_lines[-1]).group(1) if err_lines else "?")
            nwarn = (re.search(r"(\d+) Warning\(s\)", err_lines[-1]).group(1) if err_lines else "?")
            if nerr == "0":
                print(f"  PASS  {prof}   ({err_lines[-1].strip() if err_lines else ''})")
                if nwarn not in ("0", "?"):
                    wl = sorted({l.strip()[:150] for l in txt.splitlines()
                                 if re.search(r"warning:", l, re.I) and "Warning(s)" not in l})
                    print(f"          ↓ {nwarn} 个告警（未启用模块的常规告警，逐条列出以便判断是否为真问题）:")
                    for l in wl[:14]:
                        print(f"          {l}")
            else:
                fails.append(prof)
                print(f"  FAIL  {prof}   ({err_lines[-1].strip() if err_lines else 'no log'})")
                for l in txt.splitlines():
                    if ": error:" in l:
                        print(f"          {l.strip()[:160]}")
        finally:
            for f in (tmp_proj, log):
                if os.path.exists(f):
                    os.remove(f)
            shutil.rmtree(os.path.join(MDK, "_profile_build"), ignore_errors=True)
    return 1 if fails else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--fast", action="store_true", help="gcc 语法检查（默认）")
    ap.add_argument("--headers", action="store_true", help="V11：契约头单独 include（不带任何 FEATURE 定义）")
    ap.add_argument("--real", action="store_true", help="UV4 真编（临时工程）")
    ap.add_argument("--only", default="", help="只跑指定组合，如 MINIMAL 或 PROFILE_MINIMAL")
    args = ap.parse_args()

    only = None
    if args.only:
        only = {p if p.startswith("PROFILE_") else f"PROFILE_{p.upper()}" for p in
                re.split(r"[,; ]+", args.only.strip())}
    rc = check_real(only) if args.real else (check_headers() if args.headers else check_fast(only))
    print("\n组合编译矩阵:", "ALL PASS" if rc == 0 else "FAILED")
    return rc


if __name__ == "__main__":
    sys.exit(main())
