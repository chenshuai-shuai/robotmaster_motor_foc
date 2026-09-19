/*
 * tools/host_shim/windows.h - **宿主机扫描专用**的 windows.h 替身（不参与固件编译）
 *
 * 背景：FatFs R0.12c 的 integer.h 用 `#ifdef _WIN32` 区分"开发平台/嵌入式平台"，宿主机 gcc
 *   （mingw）天生带 _WIN32 → 会去 include 真正的 <windows.h>，而它定义了 `ERROR` / `BYTE` /
 *   `DWORD` 等宏与类型，和 stm32f4xx.h 的 `ErrorStatus` 枚举、工程里的 BYTE/DWORD typedef
 *   直接撞车（实测签名：`stm32f4xx.h:200:3: error: expected identifier before numeric constant`）。
 *
 * 目标（ARMCLANG）根本没有 _WIN32 → 走嵌入式分支。所以宿主机扫描必须"把平台掰回目标"：
 *   tools/check_profiles.py 把这个目录放在 -I 列表最前面 → `#include <windows.h>` 命中本文件。
 *
 * 这里只提供 FatFs integer.h 的"开发平台"分支真正需要的那几个类型（它只用 QWORD，
 * 其余是 FatFs 其它文件/示例会用到的常规 Windows 定宽类型），**故意不定义 ERROR 之类宏**。
 */
#ifndef HOST_SHIM_WINDOWS_H
#define HOST_SHIM_WINDOWS_H

typedef int INT;
typedef unsigned int UINT;
typedef unsigned char BYTE;
typedef short SHORT;
typedef unsigned short WORD;
typedef unsigned short WCHAR;
typedef long LONG;
typedef unsigned long DWORD;
typedef unsigned long long QWORD;

#endif /* HOST_SHIM_WINDOWS_H */
