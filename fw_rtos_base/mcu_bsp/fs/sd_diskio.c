/**
 * @file    sd_diskio.c
 * @brief   FatFs 磁盘 I/O 层：对接 HAL_SD 块读写（SDIO 4-bit）
 *
 * FatFs 的 diskio.h 声明 5 个函数（disk_status/disk_initialize/disk_read/
 * disk_write/disk_ioctl），本文件是它们的 HAL_SD 实现。
 * 卷号固定 0（FF_VOLUMES=1），SD 卡扇区恒为 512 字节。
 */
#include "diskio.h"
#include "bsp_log.h"
#include <string.h>
#include "sd_sdio.h"

/* 驱动器号 0 的状态 */
static volatile DSTATUS s_stat = STA_NOINIT;

/* 写块计数：格式化进度显示 + 卡死定位（卡死时日志最后显示的计数 = 卡死块位置） */
static volatile uint32_t s_write_count = 0U;

uint32_t sd_diskio_get_write_count(void)
{
    return s_write_count;
}

/* FatFs 文件时间戳：板子无 RTC，返回固定时间 2026-01-01 00:00:00。
 * 格式：bit31:25=年(自1980) bit24:21=月 bit20:16=日 bit15:11=时 bit10:5=分 bit4:0=秒/2 */
DWORD get_fattime(void)
{
    return ((DWORD)(2026U - 1980U) << 25) | (1UL << 21) | (1UL << 16);
}

/* ---- diskio 实现 ---- */

DSTATUS disk_status(BYTE pdrv)
{
    if (pdrv != 0)
    {
        return STA_NOINIT;
    }
    return s_stat;
}

DSTATUS disk_initialize(BYTE pdrv)
{
    if (pdrv != 0)
    {
        return STA_NOINIT;
    }
    if (SD_SDIO_Init() == HAL_OK)
    {
        s_stat = 0U;
    }
    else
    {
        s_stat = STA_NOINIT;
    }
    return s_stat;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, DWORD sector, UINT count)
{
    if (pdrv != 0)
    {
        return RES_PARERR;
    }
    if ((buff == NULL) || (count == 0U))
    {
        return RES_PARERR;
    }
    if (s_stat & STA_NOINIT)
    {
        return RES_NOTRDY;
    }

    if (SD_SDIO_ReadBlocks(buff, (uint32_t)sector, count) == HAL_OK)
    {
        return RES_OK;
    }
    LOG_E("diskio", "disk_read FAIL at sector=%lu cnt=%u errcode=0x%lX",
          (unsigned long)sector, (unsigned)count, (unsigned long)hsd.ErrorCode);
    return RES_ERROR;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, DWORD sector, UINT count)
{
    if (pdrv != 0)
    {
        return RES_PARERR;
    }
    if ((buff == NULL) || (count == 0U))
    {
        return RES_PARERR;
    }
    if (s_stat & STA_NOINIT)
    {
        return RES_NOTRDY;
    }

    /* 写前日志（诊断 FR_NO_FILE：观察目录扇区/FAT/数据扇区各写了什么） */
    LOG_D("diskio", "disk_write sector=%lu count=%u", (unsigned long)sector, count);

    if (SD_SDIO_WriteBlocks(buff, (uint32_t)sector, count) == HAL_OK)
    {
        /* 测试期诊断：写后立刻读回全扇区比对，抓"假成功" */
        static uint8_t vb[512];
        if (SD_SDIO_ReadBlocks(vb, (uint32_t)sector, 1U) == HAL_OK)
        {
            int diff_at = -1;
            for (int k = 0; k < 512; k++)
            {
                if (vb[k] != buff[k])
                {
                    diff_at = k;
                    break;
                }
            }
            if (diff_at >= 0)
            {
                LOG_E("diskio", "WRITE MISMATCH sector=%lu first-diff@%d [w:%02X %02X %02X %02X r:%02X %02X %02X %02X]",
                      (unsigned long)sector, diff_at,
                      buff[diff_at], buff[diff_at + 1], buff[diff_at + 2], buff[diff_at + 3],
                      vb[diff_at], vb[diff_at + 1], vb[diff_at + 2], vb[diff_at + 3]);
            }
        }

        s_write_count += count;
        /* 每 4096 块打一次进度：格式化卡死时日志显示最后进度块号 */
        if ((s_write_count & 0xFFFU) == 0U)
        {
            LOG_I("diskio", "write progress: %lu blocks (sector %lu)",
                  (unsigned long)s_write_count, (unsigned long)sector);
        }
        return RES_OK;
    }
    return RES_ERROR;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    if (pdrv != 0)
    {
        return RES_PARERR;
    }
    if (s_stat & STA_NOINIT)
    {
        return RES_NOTRDY;
    }

    switch (cmd)
    {
        case CTRL_SYNC:         /* 刷缓存（块模式无缓存，直接返回） */
            return RES_OK;

        case GET_SECTOR_COUNT:  /* 总扇区数（f_mkfs/f_getfree 需要） */
            *(DWORD *)buff = (DWORD)(hsd.SdCard.BlockNbr);
            return RES_OK;

        case GET_SECTOR_SIZE:   /* 扇区大小 */
            *(WORD *)buff = (WORD)(hsd.SdCard.BlockSize);
            return RES_OK;

        case GET_BLOCK_SIZE:    /* 擦除块大小（f_mkfs 对齐用） */
            *(DWORD *)buff = 128UL; /* SD 卡典型擦除块 64KB */
            return RES_OK;

        default:
            return RES_PARERR;
    }
}
