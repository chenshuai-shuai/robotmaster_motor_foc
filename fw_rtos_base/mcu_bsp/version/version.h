/**
 * @file    version.h
 * @brief   固件版本号（双版本体系，防止新旧固件混淆）
 *
 * 版本策略（2026-08-20 用户定）：
 *   - FW_VERSION_STR 测试版：每次修改代码交付前必须 bump
 *     （python tools/bump_version.py 自动递增修订位）
 *   - FW_RELEASE_STR 发布版：仅测试通过、提交时手动更新（V0.01 → V0.02 ...）
 *   平时改动只动测试版，发布版保持不动。
 *
 * 用法：main 启动 LOG_I 双版本打印 + OLED 首行显示。
 */
#ifndef __VERSION_H
#define __VERSION_H

#define FW_VERSION_STR    "v0.1.40-dev"   /* 测试开发版（bump_version.py 自动递增） */
#define FW_RELEASE_STR    "V0.02.1"       /* 发布版（仅测试通过提交时手动更新）：CLI 终端交互补丁 */
#define FW_BUILD_DATE     __DATE__        /* 编译器内置：编译日期 */
#define FW_BUILD_TIME     __TIME__        /* 编译器内置：编译时间 */

#endif /* __VERSION_H */
