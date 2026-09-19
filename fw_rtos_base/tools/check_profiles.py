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
# 宿主机 gcc（mingw）**自带 _WIN32**，目标（ARMCLANG）没有 → FatFs 的 integer.h 会走"开发平台"
# 分支去 include <windows.h>（其 ERROR/BYTE/DWORD 与 stm32f4xx.h 的 ErrorStatus 枚举撞车）。
# 处置：把这个 shim 目录放在 -I 最前面，让 <windows.h> 命中替身（只给类型、不给宏）。
# 不能简单 -U_WIN32 —— mingw 自己的 <stdio.h> 等头文件会 #error "Only Win32 target is supported!"。
HOST_SHIM = ["-Itools/host_shim"]

PROFILES = ["PROFILE_FULL", "PROFILE_DISP_DEV", "PROFILE_MOTOR_DEV", "PROFILE_MINIMAL"]

INCLUDES = [
    "Core/Inc", "Drivers/STM32F4xx_HAL_Driver/Inc", "Drivers/STM32F4xx_HAL_Driver/Inc/Legacy",
    "Drivers/CMSIS/Device/ST/STM32F4xx/Include", "Drivers/CMSIS/Include",
    "Task/Inc", "Device/Inc",
    "mcu_bsp/can", "mcu_bsp/Motor", "mcu_bsp/ctrl", "mcu_bsp/proto", "mcu_bsp/key",
    "mcu_bsp/uart", "mcu_bsp/log", "mcu_bsp/oled", "mcu_bsp/sd", "mcu_bsp/fs",
    "mcu_bsp/version", "mcu_bsp/sys_status", "mcu_bsp/cli", "mcu_bsp/button",
    "mcu_bsp/protocol", "mcu_bsp/protocol_calibration", "mcu_bsp/disp",
    "Middlewares/Third_Party/FreeRTOS/Source/include",
    "Middlewares/Third_Party/FreeRTOS/Source/CMSIS_RTOS",
    "Middlewares/Third_Party/FreeRTOS/Source/portable/GCC/ARM_CM4F",
    # FatFs（uvprojx 里有，这里漏了 → 之前被"路径错了但报错过滤漏掉 fatal error"掩盖）
    "Middlewares/Third_Party/FatFs/src", "Middlewares/Third_Party/FatFs/src/option",
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
    # 显示抽象层（S1/S2）：契约头 + 私有头 + 纯逻辑头都要能单独 include
    "mcu_bsp/disp/disp_port.h", "mcu_bsp/disp/disp_geom.h", "mcu_bsp/disp/disp_st7735s_spi.h",
]

# 显示驱动变体（在某个 profile 之上再覆盖 §4 的驱动宏）：
# 彩屏驱动是**硬件选择**、不随 profile 变 → 不这样显式覆盖的话，ST7735S 的实现文件会被 #if 包成
# 空 TU，"写好了却从没被编译过"。S2 烧板前必须先过这一关（uvprojx/IncludePath/头文件依赖都被真实解析）。
DRIVER_VARIANTS = [
    # 两份都列：默认选哪块屏都不影响"两个驱动的函数体都被真编过"（换默认值不该让另一个漏检）
    ("DISP_ST7735S", ["CFG_PROFILE=PROFILE_DISP_DEV",
                      "FEATURE_DISP_SH1106_I2C=0", "FEATURE_DISP_ST7735S_SPI=1"]),
    ("DISP_SH1106", ["CFG_PROFILE=PROFILE_DISP_DEV",
                     "FEATURE_DISP_SH1106_I2C=1", "FEATURE_DISP_ST7735S_SPI=0"]),
]

# "真的有代码"检查：宏包住的驱动文件在**启用它的那份配置**下必须编译出实体（不是空对象）。
# 抓的是这类事故：`#if FEATURE_X` 写在 `#include` 之前 → 宏未定义按 0 处理 → 整个文件静默变空，
# 语法扫描一片绿（空 TU 合法），直到链接期才报 L6218E（2026-09-19 实踩）。
# 做法：用同样的 -D 做预处理（-E -P），断言**只出现在函数体里**的字符串还在。
BODY_CHECKS = [
    # 注意：这里**显式给两份驱动宏**，不依赖 §4 的默认选谁（换屏后门禁不该跟着变红）
    ("SH1106 驱动", ["CFG_PROFILE=PROFILE_DISP_DEV",
                    "FEATURE_DISP_SH1106_I2C=1", "FEATURE_DISP_ST7735S_SPI=0"],
     "mcu_bsp/disp/disp_sh1106_i2c.c", "selftest=%u flush="),
    ("ST7735S 驱动", ["CFG_PROFILE=PROFILE_DISP_DEV",
                      "FEATURE_DISP_SH1106_I2C=0", "FEATURE_DISP_ST7735S_SPI=1"],
     "mcu_bsp/disp/disp_st7735s_spi.c", "font rom probe"),
]


def rel(p):
    """BASE 相对路径（**与进程 CWD 无关**）。

    坑（2026-09-19 实测）：原来直接写 os.path.relpath(相对串, BASE) —— 相对串会先按**进程 CWD**
    解析再算相对，于是"从仓库根目录跑 make profiles"时得到 `..\\Core\\Inc` 这类路径，gcc 找不到文件；
    而 --fast 的报错过滤只认 ": error:"，**漏掉了 cc1 的 "fatal error:"** → 整段静默假绿。
    现在：绝对路径才 relpath，相对串原样用（正斜杠）。"""
    if os.path.isabs(p):
        return os.path.relpath(p, BASE).replace("\\", "/")
    return p.replace("\\", "/")


def gcc_errors(text):
    """gcc 错误行（必须同时认 ": error:" 与 "fatal error:"/"cc1.exe: fatal error:"）"""
    out = []
    for l in (text or "").splitlines():
        if (": error:" in l) or ("fatal error:" in l) or ("cc1.exe: error" in l):
            out.append(l)
    return out


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
            # 写临时文件加重试：Windows 上杀软/索引器偶尔短暂锁住刚写的文件，
            # 会以 PermissionError 让整条门禁假红（2026-09-19 实测一次）——重试 5 次、间隔 200ms。
            for _try in range(5):
                try:
                    with open(tu, "w", encoding="utf-8", newline="") as fh:
                        fh.write(f'#include "{h}"\n')
                    break
                except PermissionError:
                    if _try == 4:
                        raise
                    import time as _t

                    _t.sleep(0.2)
            cmd = ([GCC, "-fsyntax-only", "-std=c99", "-DUSE_HAL_DRIVER", "-DSTM32F427xx", "-I."] + HOST_SHIM
                   + [f"-I{rel(i)}" for i in INCLUDES]
                   + [rel(tu)])
            r = subprocess.run(cmd, cwd=BASE, capture_output=True, text=True,
                               encoding="utf-8", errors="replace")
            if r.returncode != 0:
                fails.append(h)
                msg = gcc_errors((r.stderr or "") + (r.stdout or ""))
                print(f"  FAIL  {h}: {(msg[0].strip() if msg else 'syntax error')[:130]}")
            else:
                print(f"  PASS  {h}")
    finally:
        shutil.rmtree(tmpd, ignore_errors=True)
    return 1 if fails else 0


def _syntax_scan(srcs, defines):
    """对给定的 -D 定义集扫一遍源文件，返回 [(文件, 首条错误)]"""
    bad = []
    for s in srcs:
        cmd = [GCC, "-fsyntax-only", "-std=c99", "-DUSE_HAL_DRIVER", "-DSTM32F427xx"] + HOST_SHIM
        cmd += [f"-D{d}" for d in defines]
        cmd += [f"-I{rel(i)}" for i in INCLUDES]
        cmd.append(rel(s))
        r = subprocess.run(cmd, cwd=BASE, capture_output=True, text=True,
                           encoding="utf-8", errors="replace")
        errs = gcc_errors(r.stderr)
        if errs:
            bad.append((os.path.relpath(s, BASE), errs[0]))
    return bad


def preprocessed_has(defines, src, marker):
    """用 -E -P 预处理：断言"只出现在函数体里"的字符串仍然存在（见 BODY_CHECKS 注释）"""
    cmd = ([GCC, "-E", "-P", "-std=c99", "-DUSE_HAL_DRIVER", "-DSTM32F427xx"] + HOST_SHIM
           + [f"-D{d}" for d in defines] + [f"-I{rel(i)}" for i in INCLUDES] + [rel(src)])
    r = subprocess.run(cmd, cwd=BASE, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if r.returncode != 0:
        return None
    return marker in (r.stdout or "")


def check_fast(only=None):
    if not os.path.exists(GCC):
        print(f"  SKIP  gcc 不存在：{GCC}")
        return 0
    srcs = sources()
    print(f"---- --fast: gcc -fsyntax-only ×{len(srcs)} 个源文件 × {len(PROFILES)} 个组合 "
          f"+ {len(DRIVER_VARIANTS)} 个驱动变体 ----")
    fails = []
    for prof in PROFILES:
        if only and prof not in only:
            continue
        bad = _syntax_scan(srcs, [f"CFG_PROFILE={prof}"])
        if bad:
            fails.append(prof)
            print(f"  FAIL  {prof}: {len(bad)} 个文件语法错误")
            for f, e in bad[:5]:
                print(f"          {f}: {e.strip()[:150]}")
        else:
            print(f"  PASS  {prof}")
    # 驱动变体：只在 -D 覆盖下才会被真编的驱动（见 DRIVER_VARIANTS 注释）
    for name, defs in DRIVER_VARIANTS:
        bad = _syntax_scan(srcs, defs)
        if bad:
            fails.append(name)
            print(f"  FAIL  驱动变体 {name}: {len(bad)} 个文件语法错误")
            for f, e in bad[:5]:
                print(f"          {f}: {e.strip()[:150]}")
        else:
            print(f"  PASS  驱动变体 {name}（{' '.join('-D' + d for d in defs)}）")
    # "真的有代码"：宏包住的驱动在启用它的配置下不许是空对象（见 BODY_CHECKS 注释）
    for label, defs, src_f, marker in BODY_CHECKS:
        has = preprocessed_has(defs, src_f, marker)
        if has is None:
            fails.append(f"BODY:{label}")
            print(f"  FAIL  {label}: 预处理失败（{src_f}）")
        elif has:
            print(f"  PASS  {label}: 驱动实体存在（{src_f} 的 {marker!r} 在预处理结果里）")
        else:
            fails.append(f"BODY:{label}")
            print(f"  FAIL  {label}: {src_f} 被编成空对象！检查 #if 是否写在 #include 之前 / 宏名是否写错")
    return 1 if fails else 0


def check_real(only=None, keep=False):
    if not os.path.exists(UV4):
        print(f"  SKIP  UV4 不存在：{UV4}")
        return 0
    src_proj = os.path.join(MDK, f"{PROJ}.uvprojx")
    if not os.path.exists(src_proj):
        print(f"  SKIP  找不到工程：{src_proj}")
        return 0
    text0 = open(src_proj, encoding="utf-8", errors="replace").read()

    # 作业表 = 4 个 profile + 驱动变体（变体的"基准 profile"见 DRIVER_VARIANTS）
    jobs = []
    for prof in PROFILES:
        if only and prof not in only:
            continue
        jobs.append((prof, [f"CFG_PROFILE={prof}"], prof))
    for name, defs in DRIVER_VARIANTS:
        base = next((d.split("=", 1)[1] for d in defs if d.startswith("CFG_PROFILE=")), "")
        if only and (name not in only) and (base not in only):
            continue
        jobs.append((name, defs, name))

    fails = []
    _nv = len([j for j in jobs if j[0] not in PROFILES])
    print(f"---- --real: 临时 uvprojx + UV4 构建 ×{len(jobs)}"
          f"（{len(jobs) - _nv} 组合 + {_nv} 驱动变体）----")
    for prof, defs, tag in jobs:
        tmp_proj = os.path.join(MDK, f"_profile_{tag}.uvprojx")
        out_dir = f"_profile_build\\{tag}\\"
        log = os.path.join(MDK, f"_profile_{tag}.log")
        # 补 -D 定义（profile / 驱动变体）+ 隔离输出目录/输出名（绝不碰正式 hex/axf）
        t = re.sub(r"<Define>(.*?)</Define>",
                   lambda m: f"<Define>{m.group(1)},{','.join(defs)}</Define>", text0, count=1)
        t = re.sub(r"<OutputDirectory>.*?</OutputDirectory>",
                   lambda m: f"<OutputDirectory>{out_dir}</OutputDirectory>", t, count=1)
        t = re.sub(r"<OutputName>.*?</OutputName>",
                   lambda m: f"<OutputName>{PROJ}_{tag}</OutputName>", t, count=1)
        os.makedirs(os.path.join(MDK, f"_profile_build", tag), exist_ok=True)
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
                print(f"  PASS  {tag}   ({err_lines[-1].strip() if err_lines else ''})")
                if nwarn not in ("0", "?"):
                    wl = sorted({l.strip()[:150] for l in txt.splitlines()
                                 if re.search(r"warning:", l, re.I) and "Warning(s)" not in l})
                    print(f"          ↓ {nwarn} 个告警（未启用模块的常规告警，逐条列出以便判断是否为真问题）:")
                    for l in wl[:14]:
                        print(f"          {l}")
            else:
                fails.append(tag)
                print(f"  FAIL  {tag}   ({err_lines[-1].strip() if err_lines else 'no log'})")
                for l in txt.splitlines():
                    if ": error:" in l:
                        print(f"          {l.strip()[:160]}")
        finally:
            if keep:
                print(f"          （--keep：产物保留在 {out_dir}）")
            else:
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
    ap.add_argument("--keep", action="store_true",
                    help="保留临时工程/输出目录（用于取某个组合的固件 hex 去烧板子，如屏调试版）")
    args = ap.parse_args()

    only = None
    if args.only:
        # 允许 --only DISP_ST7735S（驱动变体名原样保留；其它 token 补 PROFILE_ 前缀）
        _names = {n for n, _d in DRIVER_VARIANTS}
        only = set()
        for p in re.split(r"[,; ]+", args.only.strip()):
            if not p:
                continue
            only.add(p if (p.startswith("PROFILE_") or p in _names) else f"PROFILE_{p.upper()}")
    rc = (check_real(only, keep=args.keep) if args.real
          else (check_headers() if args.headers else check_fast(only)))
    print("\n组合编译矩阵:", "ALL PASS" if rc == 0 else "FAILED")
    return rc


if __name__ == "__main__":
    sys.exit(main())
