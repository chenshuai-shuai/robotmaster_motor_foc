# 8108 关节电机固件（RoboMaster A 板 / fw_rtos_base）—— 统一验证入口
#
# 本工程无 CI；以下目标即 canonical 入口（供人、脚本、自动化检查统一调用）：
#   make verify  完整核验：宿主机纯逻辑测试 + 静态断言 + UV4 全量构建 + 链接 MAP 证据
#   make fast    跳过 UV4 构建（只跑逻辑/静态检查，秒级）
#   make build   仅全量构建固件（生成 hex）
#
# 依赖：python（含 pymupdf/pyserial 等已在用）、msys2 mingw64 gcc、Keil UV4（见 tools/verify_dev.py 顶部路径）

PY   ?= python
ROOT := fw_rtos_base
MDK  := $(ROOT)/MDK-ARM
PROJ := F427IIH6_CAN
UV4  ?= /e/keil_arm/AppData/Local/Keil_v5/UV4/UV4.exe

# msys2 make 下 TMP/TEMP 常为空 → 宿主机 gcc 会尝试写 C:\WINDOWS 而失败；显式指到可写目录
export TMP  := $(shell cygpath -w /tmp 2>/dev/null || echo .)
export TEMP := $(TMP)

.PHONY: verify fast build bench help profiles profiles-full headers

help:
	@echo "targets: verify (full) | fast (no build) | build (firmware only) | bench (台架串口冒烟)"
	@echo "         profiles (组合门禁: 4 profile 语法 + 契约头单独 include) | profiles-full (+ UV4 真编 ×4)"
	@echo "台架测试手册: $(ROOT)/docs/测试手册_8108串口控制.md"

verify:
	$(PY) $(ROOT)/tools/verify_dev.py

fast:
	$(PY) $(ROOT)/tools/verify_dev.py --fast

build:
	cd $(MDK) && "$(UV4)" -r $(PROJ).uvprojx -j0 -t $(PROJ) -o rebuild_verify.log
	@grep -E "0 Error\(s\)|Program Size" $(MDK)/rebuild_verify.log || true

# 台架串口冒烟（L0/L2：不接电机也能跑；判据与手册 §3 对应）
# 用前先关掉串口助手；PORT 默认 COM3：make bench PORT=COM5
PORT ?= COM3
bench:
	$(PY) $(ROOT)/tools/serial_bench.py --port $(PORT) --smoke

# 组合门禁（V9–V13）：日常用 profiles（秒级），交付前用 profiles-full（含 UV4 真编 ×4，约 4 分钟）
profiles:
	$(PY) $(ROOT)/tools/check_profiles.py --fast
	$(PY) $(ROOT)/tools/check_profiles.py --headers

profiles-full:
	$(PY) $(ROOT)/tools/check_profiles.py --fast
	$(PY) $(ROOT)/tools/check_profiles.py --headers
	$(PY) $(ROOT)/tools/check_profiles.py --real

headers:
	$(PY) $(ROOT)/tools/check_profiles.py --headers
