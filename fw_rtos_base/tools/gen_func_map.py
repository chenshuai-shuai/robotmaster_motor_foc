#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_func_map.py — 生成 docs/code/_函数地图.md（全工程函数清单，供文档覆盖核对）

为什么需要它：第一版扫描器把「返回类型」写成了硬编码白名单，导致
非 void/指针返回的函数（如 `J8108_Res_e J8108_ReqResult(void)`、
`const Ctrl_State_t *J8108_State(void)`、`const Ui_Status_t *Monitor_Ui(void)`）
被漏收 → 文档里写了它们反被验收器判成「编造」。

本版判据（与返回类型无关）：
  · 一行形如  <修饰符*> <返回类型> <函数名>( <参数> )   且不以 ';' 结尾（排除原型声明）
  · 排除控制关键字行（if/for/while/switch/return/typedef/struct/enum/case…）
  · 参数跨行时先拼接括号平衡的行
  · 同时收录 .h 声明与 .c 定义（同名去重时优先 .c）

用法：python tools/gen_func_map.py
"""
import io
import os
import re
import sys

BASE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(BASE, "docs", "code", "_函数地图.md")

SRC_DIRS = ["Core/Src", "Core/Inc", "Task/Src", "Task/Inc", "mcu_bsp", "Device"]
SKIP_DIRS = ("Drivers", "Middlewares", "USB_DEVICE")

KEYWORDS = {
    "if", "for", "while", "switch", "return", "else", "do", "typedef", "struct", "union", "enum", "case",
    "sizeof", "goto", "break", "continue", "default", "static", "const", "void", "int", "char", "float",
    "double", "long", "short", "unsigned", "signed", "volatile", "inline", "extern", "register",
}

# 判据：取 '(' 前最后一个标识符作为函数名，且它前面必须有「返回类型」（否则是调用）
def name_of_signature(line):
    p = line.find("(")
    if p <= 0:
        return None
    head = line[:p]
    m = re.search(r"([A-Za-z_]\w*)\s*$", head)
    if not m:
        return None
    name = m.group(1)
    if name in KEYWORDS:
        return None
    tpart = head[:m.start()].strip()
    if not tpart or any(c in tpart for c in "=;,{}()[]"):
        return None
    if not (tpart[0].isalpha() or tpart[0] == "_"):
        return None
    return name


def strip_comment(line):
    out = []
    i = 0
    while i < len(line):
        if line.startswith("//", i):
            break
        if line.startswith("/*", i):
            j = line.find("*/", i + 2)
            if j < 0:
                break
            i = j + 2
            continue
        out.append(line[i])
        i += 1
    return "".join(out)


def scan_file(path):
    """返回 [(行号, 函数名)]"""
    lines = io.open(path, encoding="utf-8", errors="replace", newline="").read().split("\n")
    funcs = []
    i = 0
    while i < len(lines):
        s = strip_comment(lines[i]).rstrip()
        if not s or s.lstrip().startswith("#") or s.endswith(";"):
            i += 1
            continue
        joined = s
        start = i
        while joined.count("(") > joined.count(")") and i + 1 < len(lines) and i - start < 10:
            i += 1
            joined = joined.rstrip() + " " + strip_comment(lines[i]).strip()
        if joined.rstrip().endswith(")") and ";" not in joined:
            nm = name_of_signature(joined)
            if nm:
                funcs.append((start + 1, nm))
        i += 1
    return funcs


def main():
    rows = []
    for d in SRC_DIRS:
        root = os.path.join(BASE, d.replace("/", os.sep))
        if not os.path.isdir(root):
            continue
        for dirpath, dirs, files in os.walk(root):
            dirs[:] = [x for x in dirs if x not in SKIP_DIRS]
            for fn in sorted(files):
                if not fn.endswith((".c", ".h")):
                    continue
                p = os.path.join(dirpath, fn)
                rel = os.path.relpath(p, BASE).replace("\\", "/")
                funcs = scan_file(p)
                if funcs:
                    rows.append((rel, len(io.open(p, encoding="utf-8", errors="replace").readlines()), funcs))

    # 同名函数：.h 声明只保留一次（若 .c 中已定义则标注）
    total = sum(len(f) for _, _, f in rows)
    uniq = {f for _, _, fs in rows for _, f in fs}

    defined = {f for rel, _, fs in rows if rel.endswith(".c") for _, f in fs}
    md = ["# 工程函数地图（自动生成，用于文档覆盖核对）", "",
          f"> 生成脚本：`tools/gen_func_map.py`（返回类型无关的扫描器，含跨行参数拼接）。",
          f"> 统计：{len(rows)} 个文件 / {total} 个函数位置 / {len(uniq)} 个唯一函数名。",
          "> 文档中提到的函数应当都能在本表找到（否则视为编造）；负责范围内的函数应当被文档覆盖（否则视为遗漏）。", ""]
    for rel, n, funcs in rows:
        md += [f"## {rel}  ({n} 行, {len(funcs)} 函数)", "", "| 行 | 函数 | 定义 |", "|---|---|---|"]
        for ln, f in funcs:
            is_def = "★" if (rel.endswith(".c") and f in defined) else ""
            md += [f"| {ln} | `{f}` | {is_def} |"]
        md += [""]
    io.open(OUT, "w", encoding="utf-8", newline="").write("\n".join(md))
    print(f"已生成 {OUT}")
    print(f"  文件 {len(rows)} / 函数位置 {total} / 唯一名 {len(uniq)}")
    # 自检：本次新增（相对旧版硬编码白名单）应当包含这些
    for probe in ("J8108_ReqResult", "J8108_State", "J8108_Params", "Monitor_Ui", "Ui_ActionDecide"):
        hit = [rel for rel, _, fs in rows if any(f == probe for _, f in fs)]
        print(f"  {probe:18s} -> {hit if hit else '!! 仍未收录'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
