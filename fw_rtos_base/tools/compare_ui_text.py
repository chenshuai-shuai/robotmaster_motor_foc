#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""一次性对照脚本：新旧 Oled_Task.c 的"上屏文本"逐条比对（S1 回归判据的物证）

用法：python tools/_compare_ui_text.py <旧文件路径>
判据：两版 rfmt() 的**文本格式串**（每条 rfmt 调用里的第一个字符串字面量）完全相同 = 逐字一致。
（rfmt 的参数个数两版不同：旧版 (row, force, fmt, ...)，新版 (row, force, fg, fmt, ...)，
 所以不能按参数位取，要按"调用体内的第一个字面量"取。）
"""
import re
import sys

LIT = re.compile(r'"((?:[^"\\]|\\.)*)"')


def rfmt_formats(path):
    txt = open(path, encoding="utf-8", errors="replace").read()
    out = []
    for m in re.finditer(r"\brfmt\s*\(", txt):
        i = m.end()
        depth = 1
        instr = False
        esc = False
        while i < len(txt) and depth > 0:
            c = txt[i]
            if instr:
                if esc:
                    esc = False
                elif c == "\\":
                    esc = True
                elif c == '"':
                    instr = False
            else:
                if c == '"':
                    instr = True
                elif c == "(":
                    depth += 1
                elif c == ")":
                    depth -= 1
            i += 1
        body = txt[m.end():i]
        lits = LIT.findall(body)
        if lits:
            out.append(lits[0])
    return out


def main():
    old = rfmt_formats(sys.argv[1])
    new = rfmt_formats("fw_rtos_base/Task/Src/Oled_Task.c")
    print("旧版上屏文本条数:", len(old), " 新版:", len(new))
    only_old = sorted(set(old) - set(new))
    only_new = sorted(set(new) - set(old))
    print("旧有新无:", only_old)
    print("新有旧无:", only_new)
    same = old == new
    print("顺序与内容逐条一致:", same)
    if not same and not only_old and not only_new:
        print("（集合相同、顺序不同 → 逐条对照如下）")
        for a, b in zip(old, new):
            if a != b:
                print("  差异:", repr(a), "vs", repr(b))
    return 0 if same else 1


if __name__ == "__main__":
    sys.exit(main())
