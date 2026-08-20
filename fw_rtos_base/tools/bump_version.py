#!/usr/bin/env python
"""bump_version.py - 固件版本号自动递增（防漏改）

规则（用户铁律 2026-08-20）：每次代码修改交付前必须 bump 版本号。
用法：python tools/bump_version.py
行为：读 mcu_bsp/version/version.h，把 vX.Y.N-阶段 的 N 自动 +1。
"""
import re
import os
import sys

BASE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VH = os.path.join(BASE, "mcu_bsp", "version", "version.h")

t = open(VH, encoding="utf-8").read()
m = re.search(r'#define FW_VERSION_STR\s+"v(\d+)\.(\d+)\.(\d+)(-[^"]*)?"', t)
if not m:
    print("❌ 版本宏格式不识别，请手改")
    sys.exit(1)

major, minor, patch, stage = m.group(1), m.group(2), int(m.group(3)), m.group(4) or ""
new_patch = patch + 1
old_str = f'v{major}.{minor}.{patch}{stage}'
new_str = f'v{major}.{minor}.{new_patch}{stage}'

t = t.replace(old_str, new_str, 1)
open(VH, "w", encoding="utf-8", newline="").write(t)
print(f"✅ 版本号 {old_str} -> {new_str}")
