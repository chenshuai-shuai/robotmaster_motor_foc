#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""serial_bench.py - 8108 串口控制台架自测（配套 docs/测试手册_8108串口控制.md）

用法：
  python tools/serial_bench.py --port COM3 --smoke        # L0/L2 层冒烟（**不需要电机上电**，安全）
  python tools/serial_bench.py --port COM3 --watch        # 只监听打印（看 @TEL/日志）
  python tools/serial_bench.py --port COM3 --interactive  # 手工交互（输命令回车发送，Ctrl-C 退出）
  python tools/serial_bench.py --port COM3 --cmd "#STAT"  # 发一条看回包
  python tools/serial_bench.py --port COM3 --smoke --unsafe-enable
        # ⚠️ 额外跑 MTR-02：会真的发使能帧（电机上电且接线正确时才会进阻尼），
        #    结束时自动 #STOP 回阻尼（不失能）。不确定就别加这个参数。

断言方式：逐条发送 → 收集 N 毫秒内的行 → 检查是否出现期望的**前缀**（同时匹配 @ 协议行与日志尾段）。
退出码 0=全过；非 0=有 FAIL（可直接用于回归）。

★ 运行时输出**一律 ASCII/英文**（中文经 make/不同代码页调用会乱码；文档保持中文）。
"""
import argparse
import sys
import time

try:
    import serial  # pyserial
except ImportError:
    print("ERROR: pyserial required -> pip install pyserial")
    sys.exit(2)

TIMEOUT_MS = 1200      # per-command reply window
QUIET_MS = 180         # after all expected lines seen, keep collecting this long (@EVT / multi-line @OK)

# (item id, command, required line fragments)
SMOKE_CHECKS = [
    ("PRT-01 PING (connectivity)",   "#PING",            ["@OK PING ms="]),
    ("PRT-01 VER (scale=output)",    "#VER",             ["@OK VER fw=", "scale=output", "pmax="]),
    ("SMK-03 STAT (status)",         "#STAT",            ["@OK STAT link="]),
    ("PRT-02 LOG quiet (lvl 0)",     "#LOG 0",           ["@OK LOG lvl=0"]),
    ("PRT-03 GET params (3 lines)",  "#GET",             ["@OK GET gains", "@OK GET limits", "@OK GET misc"]),
    ("PRT-03 SET valid param",       "#SET kd_damp 1.0", ["@OK SET KD_DAMP=1.000"]),
    ("PRT-03 SET out-of-range (Kp<=500)", "#SET kp_pos 600", ["@ERR 2 KP_POS"]),
    ("SMK-04 unknown cmd -> ERR 1",  "#FOO",             ["@ERR 1 FOO"]),
    ("SMK-04 malformed num -> ERR 2", "#V 1.2.3",        ["@ERR 2 V"]),
    ("SMK-04 missing arg -> ERR 2",  "#V",               ["@ERR 2 V usage:"]),
    ("PRT-04 LIM echo",              "#LIM",             ["@OK LIM pmin="]),
    ("PRT-04 LIM validation",        "#LIM P 90 -90",    ["@ERR 2 LIM min must be"]),
    ("PRT-06 ZERO needs CONFIRM",    "#ZERO",            ["@ERR 4 ZERO"]),
    ("PRT-06 SETP locked by default", "#SETP 0 0 0 0 0", ["@ERR 4 SETP"]),
    # 用 #V 0 而不是 #P 10：#V 0 在任何状态下都不会让关节动（未使能->报错；已使能->目标 0 dps = 阻尼）。
    # 冒烟脚本不得包含"会让关节动"的命令。
    ("PRT-08 motion cmd w/o EN (safe #V 0)", "#V 0",     ["@ERR 3 V"]),
    ("PRT-05 TEL once",              "#TEL 1",           ["@TEL ms="]),
    ("PRT-01 HELP command list",     "#HELP",            ["@OK HELP cmd="]),
    ("SMK-03 STAT recheck",          "#STAT",            ["@OK STAT link="]),
    ("PRT-02 LOG restore (lvl 1)",   "#LOG 1",           ["@OK LOG lvl=1"]),
    ("PRT-05 TEL off",               "#TEL 0",           ["@OK TEL period=0"]),
]

UNSAFE_CHECKS = [
    ("MTR-02 ENABLE (#EN)",          "#EN",              ["@OK EN mode=DAMP frames=ON"]),
    ("MTR-03 feedback stream (hold=1)", "#STAT",         ["hold=1"]),
    ("MTR-05 back to DAMP (#STOP)",  "#STOP",            ["@OK STOP mode=DAMP"]),
]


def read_lines(ser, window_ms):
    """collect all complete lines within window_ms (protocol @ lines and log lines alike)"""
    out = []
    end = time.time() + window_ms / 1000.0
    buf = b""
    while time.time() < end:
        n = ser.in_waiting
        if n:
            buf += ser.read(n)
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                out.append(raw.decode("utf-8", "replace").rstrip("\r"))
        else:
            time.sleep(0.01)
    return out


def run_check(ser, tag, cmd, want, verbose):
    ser.reset_input_buffer()
    ser.write((cmd + "\n").encode())
    lines, matched = [], set()
    end = time.time() + TIMEOUT_MS / 1000.0
    quiet_until = None
    buf = b""
    while time.time() < end:
        n = ser.in_waiting
        if n:
            buf += ser.read(n)
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                line = raw.decode("utf-8", "replace").rstrip("\r")
                lines.append(line)
                for w in want:
                    if w in line:
                        matched.add(w)
                if len(matched) == len(want) and quiet_until is None:
                    quiet_until = time.time() + QUIET_MS / 1000.0
        else:
            if quiet_until is not None and time.time() > quiet_until:
                break
            time.sleep(0.01)

    ok = len(matched) == len(want)
    print(("  PASS  " if ok else "  FAIL  ") + f"{tag}   [{cmd}]")
    if not ok:
        print(f"        expected fragments: {want}")
        print(f"        missing: {[w for w in want if w not in matched]}")
    if verbose or not ok:
        for l in lines:
            print("        | " + l)
    return ok


def watch(ser):
    while True:
        for l in read_lines(ser, 300):
            print(l)
        sys.stdout.flush()


def interactive(ser):
    print('type a command (newline added automatically); empty line quits')
    while True:
        cmd = input("> ").strip()
        if not cmd:
            return
        ser.write((cmd + "\n").encode())
        for l in read_lines(ser, 800):
            print("  " + l)


def main():
    ap = argparse.ArgumentParser(
        description="8108 serial bench runner (test manual: fw_rtos_base/docs/  TM-1)")
    ap.add_argument("--port", default="COM3")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--smoke", action="store_true", help="run L0/L2 smoke (no motor power needed)")
    ap.add_argument("--watch", action="store_true", help="just print incoming lines")
    ap.add_argument("--interactive", action="store_true", help="manual command loop")
    ap.add_argument("--cmd", default=None, help="send one command and print replies")
    ap.add_argument("--unsafe-enable", action="store_true", help="ALSO run #EN (really energizes the motor!)")
    ap.add_argument("-v", "--verbose", action="store_true", help="print every line")
    a = ap.parse_args()

    try:
        ser = serial.Serial(a.port, a.baud, timeout=0.05)
    except Exception as e:  # noqa: BLE001
        # Windows 的系统错误消息可能含中文；经 make/不同代码页调用会乱码 → 净化成 ASCII
        msg = str(e).encode("ascii", "replace").decode("ascii")
        print(f"ERROR: cannot open {a.port}: {msg}")
        print("       (close any serial terminal / IDE monitor that holds the port; check --port)")
        return 2

    print(f"serial_bench: {a.port} @{a.baud} 8N1")
    rc = 0
    try:
        if a.watch:
            print("(Ctrl-C to quit)")
            watch(ser)
        elif a.interactive:
            interactive(ser)
        elif a.cmd:
            ser.write((a.cmd + "\n").encode())
            for l in read_lines(ser, TIMEOUT_MS):
                print(l)
        else:
            checks = list(SMOKE_CHECKS)
            if a.unsafe_enable:
                print("WARNING: --unsafe-enable -> the ENABLE frame will really be sent")
                checks += UNSAFE_CHECKS
            print("---- smoke (runs without motor power; power/wiring faults surface as @ERR) ----")
            for tag, cmd, want in checks:
                if not run_check(ser, tag, cmd, want, a.verbose):
                    rc = 1
            print("\nresult: " + ("ALL PASS" if rc == 0 else "FAIL (see above)"))
    except KeyboardInterrupt:
        pass
    finally:
        ser.close()
    return rc


if __name__ == "__main__":
    sys.exit(main())
