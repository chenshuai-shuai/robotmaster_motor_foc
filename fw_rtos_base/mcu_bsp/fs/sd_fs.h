/**
 * @file    sd_fs.h
 * @brief   SD 卡文件系统应用层：检测 → 初始化 → 挂载/格式化 → 容量查询 → 读写测试
 *
 * 策略（用户需求）：f_mount 失败（无文件系统）→ f_mkfs 格式化 FAT32 → 重新挂载。
 * 32GB 卡 FAT32 完全够用（FAT32 技术上支持至 2TB）。
 * 单任务独占访问（SdCard_Task），未开 FatFs 重入（FF_FS_REENTRANT=0）。
 */
#ifndef __SD_FS_H
#define __SD_FS_H

#include "ff.h"
#include <stdint.h>

FRESULT SD_FS_Mount(void);           /* 挂载；无 FAT 则格式化 FAT32 后重挂 */
void SD_FS_Unmount(void);            /* 卸载（f_mount(NULL)） */
uint32_t SD_FS_TotalMB(void);        /* 总容量（MB）；失败返回 0 */

/**
 * @brief 读写测试：写 test.txt（含版本信息）→ 读回 → 逐字节校验。
 * @param out_read_bytes  输出：读回字节数（校验通过时）
 * @return 0=通过；非 0=FRESULT 错误码或 -1=内容不一致
 */
int SD_FS_RWTest(uint32_t *out_read_bytes);

#endif /* __SD_FS_H */
