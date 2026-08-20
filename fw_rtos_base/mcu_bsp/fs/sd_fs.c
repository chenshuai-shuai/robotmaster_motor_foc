/**
 * @file    sd_fs.c
 * @brief   SD 卡文件系统应用层实现
 *
 * 流程：f_mount 尝试挂载 → 失败则 f_mkfs(FAT32) 格式化 → 再挂载 → 容量汇报。
 * 注意：f_mkfs 工作缓冲 512B，用 static（任务栈放不下）。
 */
#include "sd_fs.h"
#include "sys_status.h"
#include "sd_sdio.h"
#include "version.h"
#include <string.h>
#include "bsp_log.h"

FATFS SDFatFs;

/* f_mkfs 工作缓冲（FatFs 要求 >= _MAX_SS 字节） */
static BYTE s_mkfs_work[_MAX_SS];

FRESULT SD_FS_Mount(void)
{
    FRESULT fr;

    /* opt=1：立即挂载（opt=0 是延迟挂载，不读盘就返回 OK，会掩盖真实错误） */
    fr = f_mount(&SDFatFs, "", 1);
    if (fr == FR_OK)
    {
        LOG_I("sdfs", "mount OK (already formatted)");
        return FR_OK;
    }

    /* 无文件系统（新卡 / exFAT 卡——Windows 对 32GB 卡默认格式化为 exFAT，
     * FatFs R0.12c 不支持 exFAT）：格式化 FAT32 后重挂 */
    if (fr == FR_NO_FILESYSTEM)
    {
        LOG_I("sdfs", "no FAT filesystem (maybe exFAT), formatting as FAT32...");
        SYS_SetState(SYS_STATE_SD_FORMAT);
        LOG_W("sdfs", "ALL DATA ON CARD WILL BE ERASED!");
        fr = f_mkfs("", FM_FAT32, 0, s_mkfs_work, sizeof(s_mkfs_work));
        if (fr != FR_OK)
        {
            LOG_E("sdfs", "f_mkfs failed (FR=%d)", (int)fr);
            return fr;
        }
        SYS_SetState(SYS_STATE_SD_MOUNT);
        LOG_I("sdfs", "format done, remounting...");
        fr = f_mount(&SDFatFs, "", 1);
        if (fr != FR_OK)
        {
            LOG_E("sdfs", "remount after format failed (FR=%d)", (int)fr);

            /* 诊断：读回扇区 0/63 内容，判断格式化写入是否真正落卡。
             * 期望 FAT32 引导签名 EB 58 90（扇区 0 或 MBR 分区起始处）；
             * 若读到 exFAT 签名 EB 76 90 = 写入未生效；全零 = BPB 没写好。 */
            uint8_t dbg[32];
            if (SD_SDIO_ReadBlocks(dbg, 0U, 1U) == HAL_OK)
            {
                LOG_I("sdfs", "diag sector0: %02X %02X %02X %02X | %02X %02X %02X %02X",
                      dbg[0], dbg[1], dbg[2], dbg[3], dbg[8], dbg[9], dbg[10], dbg[11]);
            }
            if (SD_SDIO_ReadBlocks(dbg, 63U, 1U) == HAL_OK)
            {
                LOG_I("sdfs", "diag sector63: %02X %02X %02X %02X | %02X %02X %02X %02X",
                      dbg[0], dbg[1], dbg[2], dbg[3], dbg[8], dbg[9], dbg[10], dbg[11]);
            }
            if (SD_SDIO_ReadBlocks(dbg, 2048U, 1U) == HAL_OK)
            {
                LOG_I("sdfs", "diag sector2048: %02X %02X %02X %02X | %02X %02X %02X %02X",
                      dbg[0], dbg[1], dbg[2], dbg[3], dbg[8], dbg[9], dbg[10], dbg[11]);
            }
        }
        return fr;
    }

    /* FR_NOT_READY（卡初始化失败）等错误原样上报 */
    LOG_E("sdfs", "mount failed (FR=%d)", (int)fr);
    return fr;
}

void SD_FS_Unmount(void)
{
    f_mount(NULL, "", 0);
}

uint32_t SD_FS_TotalMB(void)
{
    FATFS *pfs = NULL;
    DWORD free_clusters = 0U;
    FRESULT fr = f_getfree("", &free_clusters, &pfs);

    /* 调试：定位 f_getfree 失败原因（FR_* 码 + 指针） */
    LOG_D("sdfs", "f_getfree fr=%d pfs=%p free_clust=%lu sdio_blk=%lu",
          (int)fr, (void *)pfs, (unsigned long)free_clusters,
          (unsigned long)hsd.SdCard.BlockNbr);

    if ((fr != FR_OK) || (pfs == NULL))
    {
        return 0U;
    }

    /* 总容量 = (总簇数) × 簇大小（扇区） × 512 / 1MB */
    uint64_t total_bytes = (uint64_t)(pfs->n_fatent - 2U) *
                           (uint64_t)(pfs->csize) * 512ULL;
    return (uint32_t)(total_bytes / (1024ULL * 1024ULL));
}

int SD_FS_RWTest(uint32_t *out_read_bytes)
{
    static const char test_text[] =
        "RobotMaster SD RW test - firmware " FW_VERSION_STR "\r\n"
        "Hello from F427! This file verifies SD write & read path.\r\n";

    FIL f;
    UINT bw = 0U;
    UINT br = 0U;
    FRESULT fr;

    /* ---- 写测试 ---- */
    LOG_I("sdfs", "rw: opening test.txt for write (create always)...");
    fr = f_open(&f, "test.txt", FA_CREATE_ALWAYS | FA_WRITE);
    if (fr != FR_OK)
    {
        LOG_E("sdfs", "rw: f_open(w) failed (FR=%d)", (int)fr);
        return (int)fr;
    }

    fr = f_write(&f, test_text, (UINT)sizeof(test_text) - 1U, &bw);
    LOG_I("sdfs", "rw: f_write done, fr=%d bytes=%lu", (int)fr, (unsigned long)bw);

    fr = f_close(&f);
    if (fr != FR_OK)
    {
        LOG_E("sdfs", "rw: f_close(w) failed (FR=%d)", (int)fr);
        return (int)fr;
    }
    LOG_I("sdfs", "rw: write OK -> test.txt (%lu bytes)", (unsigned long)bw);

    /* ---- 读回测试 ---- */
    LOG_I("sdfs", "rw: opening test.txt for read...");
    fr = f_open(&f, "test.txt", FA_READ);
    if (fr != FR_OK)
    {
        LOG_E("sdfs", "rw: f_open(r) failed (FR=%d)", (int)fr);

        /* 诊断1：直接读目录扇区 16384 前 64 字节——看 test.txt 目录项是否在卡上 */
        uint8_t ds[64];
        if (SD_SDIO_ReadBlocks(ds, 16384U, 1U) == HAL_OK)
        {
            LOG_I("sdfs", "dir sector 16384 hex:");
            for (int k = 0; k < 64; k += 16)
            {
                LOG_I("sdfs", "  %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
                      ds[k], ds[k+1], ds[k+2], ds[k+3], ds[k+4], ds[k+5], ds[k+6], ds[k+7],
                      ds[k+8], ds[k+9], ds[k+10], ds[k+11], ds[k+12], ds[k+13], ds[k+14], ds[k+15]);
            }
        }

        /* 诊断2：列出根目录实际内容，判断文件是否真正落卡 */
        DIR dir;
        FILINFO fi;
        if (f_opendir(&dir, "/") == FR_OK)
        {
            LOG_I("sdfs", "rw: root dir listing:");
            while ((f_readdir(&dir, &fi) == FR_OK) && (fi.fname[0] != 0))
            {
                LOG_I("sdfs", "  %-13s %lu bytes", fi.fname, (unsigned long)fi.fsize);
            }
            f_closedir(&dir);
        }
        return (int)fr;
    }

    static char rbuf[128];
    fr = f_read(&f, rbuf, (UINT)sizeof(rbuf) - 1U, &br);
    if (fr != FR_OK)
    {
        f_close(&f);
        LOG_E("sdfs", "rw: f_read failed (FR=%d)", (int)fr);
        return (int)fr;
    }
    rbuf[br] = '\0';
    f_close(&f);
    LOG_I("sdfs", "rw: read OK -> %lu bytes", (unsigned long)br);
    LOG_I("sdfs", "rw: content: \"%.40s\"", rbuf);

    /* ---- 校验 ---- */
    if (out_read_bytes != NULL)
    {
        *out_read_bytes = br;
    }

    if ((br == (UINT)(sizeof(test_text) - 1U)) &&
        (memcmp(rbuf, test_text, (size_t)br) == 0))
    {
        LOG_I("sdfs", "rw: VERIFY PASS - write/read path OK");
        return 0;
    }

    LOG_E("sdfs", "rw: VERIFY FAIL - content mismatch (br=%lu)", (unsigned long)br);
    return -1;
}
