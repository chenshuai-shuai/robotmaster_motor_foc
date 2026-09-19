#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""一次性脚本：把 disp 层文件加进 Keil 工程（幂等；S1/S2 集成用）

做三件事：
  1) BSP 组插入 disp_sh1106_i2c.c / disp_st7735s_spi.c / disp_geom.c
  2) Drivers/STM32F4xx_HAL_Driver 组插入 stm32f4xx_hal_spi.c（HAL_SPI_Init 需要）
  3) 主 IncludePath 追加 ../mcu_bsp/disp（Keil 的 include 路径逐目录列举，不递归子目录）
插完做 XML 合法性 + 节点唯一性断言。用法：python tools/add_disp_to_project.py
"""
import os
import re
import sys
import xml.etree.ElementTree as ET

BASE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROJ = os.path.join(BASE, "MDK-ARM", "F427IIH6_CAN.uvprojx")

NEW_BSP = [
    ("disp_geom.c", "..\\mcu_bsp\\disp\\disp_geom.c"),
    ("disp_sh1106_i2c.c", "..\\mcu_bsp\\disp\\disp_sh1106_i2c.c"),
    ("disp_st7735s_spi.c", "..\\mcu_bsp\\disp\\disp_st7735s_spi.c"),
]
NEW_HAL = [("stm32f4xx_hal_spi.c", "../Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_spi.c")]
NEW_TASK = [("Disp_Task.c", "..\Task\Src\Disp_Task.c")]
INC_ADD = "../mcu_bsp/disp"

t = open(PROJ, encoding="utf-8", errors="replace").read()
nl = "\r\n" if t.count("\r\n") > t.count("\n") - t.count("\r\n") else "\n"
print(f"原行尾={'CRLF' if nl == chr(13)+chr(10) else 'LF'}  文件 {len(t)} 字节")


def node(fn, fp, indent):
    return (f'{indent}<File>{nl}'
            f'{indent}  <FileName>{fn}</FileName>{nl}'
            f'{indent}  <FileType>1</FileType>{nl}'
            f'{indent}  <FilePath>{fp}</FilePath>{nl}'
            f'{indent}</File>{nl}')


def insert_into_group(text, group_name, entries, marker):
    """在 group_name 组的 <Files> 里插入 entries（锚点 = 该组后第一个 </Files>）"""
    gi = text.index(f"<GroupName>{group_name}</GroupName>")
    fi = text.index("</Files>", gi)  # 本组自己的闭合（组不嵌套）
    head = text.rfind("<File>", gi, fi)
    indent = " " * 10
    if head != -1:
        line_start = text.rfind("\n", 0, head) + 1
        indent = re.match(r"[ \t]*", text[line_start:]).group(0)
    add = ""
    for fn, fp in entries:
        if f"<FileName>{fn}</FileName>" in text:
            print(f"  跳过（已存在）{fn}")
            continue
        add += node(fn, fp, indent)
        print(f"  {marker} {fn}")
    if add == "":
        return text
    line_start = text.rfind("\n", 0, fi) + 1
    return text[:line_start] + add + text[line_start:]


t = insert_into_group(t, "BSP", NEW_BSP, "BSP 组 +")
t = insert_into_group(t, "Task", NEW_TASK, "Task 组 +")

# HAL 组（注意：这里用 4 空格缩进对齐它附近的节点）
gi = t.index("<GroupName>Drivers/STM32F4xx_HAL_Driver</GroupName>")
fi = t.index("</Files>", gi)
add = ""
for fn, fp in NEW_HAL:
    if f"<FileName>{fn}</FileName>" in t:
        print(f"  跳过（已存在）{fn}")
    else:
        add += node(fn, fp, "            ")
        print(f"  HAL 组 + {fn}")
if add:
    ls = t.rfind("\n", 0, fi) + 1
    t = t[:ls] + add + t[ls:]

# IncludePath：追加到**主**（最长那条）include 路径列表末尾
m = max(re.finditer(r"<IncludePath>(.*?)</IncludePath>", t, re.S), key=lambda x: len(x.group(1)))
if INC_ADD in m.group(1):
    print("  跳过（IncludePath 已有）")
else:
    body = m.group(1)
    trimmed = body.rstrip()
    tail = body[len(trimmed):]
    sep = "" if trimmed.endswith(";") else ";"
    new = trimmed + sep + INC_ADD + tail
    t = t[:m.start(1)] + new + t[m.end(1):]
    print(f"  IncludePath + {INC_ADD}")

open(PROJ, "w", encoding="utf-8", newline="").write(t)

# ------- 验证 -------
bad = []
for fn, _fp in NEW_BSP + NEW_HAL + NEW_TASK:
    c = open(PROJ, encoding="utf-8", errors="replace").read().count(f"<FileName>{fn}</FileName>")
    if c != 1:
        bad.append(f"{fn} 节点数={c}")
root = ET.parse(PROJ).getroot()
inc = [e.text or "" for e in root.iter("IncludePath")]
if not any(INC_ADD in s for s in inc):
    bad.append("IncludePath 未写入")
print("XML 合法，节点/路径检查:", "OK" if not bad else f"FAIL {bad}")
sys.exit(1 if bad else 0)
