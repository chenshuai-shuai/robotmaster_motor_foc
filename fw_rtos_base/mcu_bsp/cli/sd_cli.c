/**
 * @file    sd_cli.c
 * @brief   SD 卡命令行实现（串口交互，类 Linux 终端体验）
 *
 * 接收链路：uart10（USART6）DMA+IDLE 接收 → HAL_UARTEx_RxEventCallback
 *          → uart10.module_callback（本文件注册）→ 双缓冲行收集 → 任务通知
 *          → CLI 任务解析执行 FatFs 命令 → 结果经日志/raw 输出。
 *
 * 命令一览（FatFs R0.12c API 对应）：
 *   help                   帮助
 *   ls  [dir]              列目录（f_opendir/f_readdir）
 *   mkdir <dir>            建目录（f_mkdir）
 *   rm   <file|dir>        删除（f_unlink；目录需为空）
 *   mv   <old> <new>       重命名/移动（f_rename）
 *   cat  <file>            打印文件内容（f_read 循环）
 *   write <file> <text...> 覆盖写文件（FA_CREATE_ALWAYS）
 *   append <file> <text...>追加写（FA_OPEN_APPEND）
 *   stat <file>            文件属性（f_stat：大小/时间）
 *   df                     容量信息（f_getfree）
 */
#include "sd_cli.h"

#include "bsp_usart.h"
#include "bsp_log.h"
#include "ff.h"
#include "sd_fs.h"
#include "sys_status.h"
#include "version.h"

#include "FreeRTOS.h"
#include "task.h"

#include <string.h>
#include <stdlib.h>

/* ---- 参数（集中配置） ---- */
#define CLI_LINE_MAX      96U     /* 单条命令行最大长度 */
#define CLI_ARGS_MAX      8U      /* 命令参数上限 */

/* ---- 接收字节环形队列（中断只入队，任务消费——中断最小化铁律） ---- */
#define CLI_RXQ_SIZE      256U              /* 2 的幂 */
static volatile uint8_t  s_rxq[CLI_RXQ_SIZE];
static volatile uint16_t s_rxq_wr = 0U;     /* 中断写指针 */
static volatile uint16_t s_rxq_rd = 0U;     /* 任务读指针 */
static volatile uint16_t s_rxq_cnt = 0U;    /* 队列字节数（诊断） */
static volatile uint16_t s_last_byte = 0U;  /* 最后收到的字节（诊断） */
static TaskHandle_t s_cli_task = NULL;

extern USARTInstance uart10;

/* ---- 回调（HAL UART IDLE/DMA 中断上下文） ----
 * 【中断最小化铁律】只做一件事：字节入环形队列。
 * 零输出、零行处理、零任务通知——回显/行编辑/命令执行全部在任务里。
 * （历史教训：中断里 log_raw 回显+通知+处理 交织，出现提示符风暴与
 *  接收死寂——2026-08-21 v0.1.35 现场） */
static void cli_rx_callback(void)
{
    /* 关键：只处理本次实际收到的字节。HAL 在 RxEventCallback 前置 RxXferCount；
     * IDLE 空触发（接收重启后线路空闲再触发）时 RxXferCount=0——若像以前遍历
     * 整个 recv_buff 会把残留旧字节重复入队（0D 风暴/丢字符根因，2026-08-21）。 */
    uint16_t n = (uint16_t)uart10.usart_handle->RxXferCount;
    if (n == 0U)
    {
        return;   /* IDLE 空触发，无新数据 */
    }

    for (uint16_t i = 0U; i < n; i++)
    {
        uint8_t c = uart10.recv_buff[i];
        if (c == '\0')
        {
            break;   /* 数据区结束 */
        }
        s_rxq[s_rxq_wr] = c;
        s_rxq_wr = (s_rxq_wr + 1U) & (CLI_RXQ_SIZE - 1U);
        if (s_rxq_cnt < CLI_RXQ_SIZE)
        {
            s_rxq_cnt++;
        }
        else
        {
            s_rxq_rd = (s_rxq_rd + 1U) & (CLI_RXQ_SIZE - 1U);  /* 满则丢最旧 */
        }
        s_last_byte = c;
        if (s_cli_task != NULL)
        {
            BaseType_t xw = pdFALSE;
            vTaskNotifyGiveFromISR(s_cli_task, &xw);
            portYIELD_FROM_ISR(xw);
        }
    }
}

/* ---- 工具 ---- */
static void cli_echo(const char *s)
{
    log_raw(s, (uint16_t)strlen(s));
}

static void cli_prompt(void)
{
    cli_echo("STM32> ");   /* 无前导换行：紧跟上一行输出（Linux 风格） */
}

static void cli_print_fr(const char *op, const char *path, FRESULT fr)
{
    LOG_I("cli", "%s %s: %s (FR=%d)", op, path,
          (fr == FR_OK) ? "OK" : "FAIL", (int)fr);
}

/* ---- 命令实现 ---- */
static void cmd_help(void)
{
    cli_echo("\r\n"
        "=== SD CLI (Linux-like shell for STM32) ===\r\n"
        " ls [dir]         list directory\r\n"
        " mkdir <dir>      make directory\r\n"
        " rm <file|dir>    remove (dir must be empty)\r\n"
        " mv <old> <new>   rename/move\r\n"
        " cat <file>       print file content\r\n"
        " write <f> <txt>  overwrite file with text\r\n"
        " append <f> <txt> append text to file\r\n"
        " stat <file>      file info (size/date)\r\n"
        " df               disk free/total\r\n"
        " help             this help\r\n");
}

static void cmd_ls(const char *path)
{
    DIR dir;
    FILINFO fi;
    uint16_t cnt = 0U;
    char buf[80];

    FRESULT fr = f_opendir(&dir, path);
    if (fr != FR_OK)
    {
        cli_print_fr("ls", path, fr);
        return;
    }

    for (;;)
    {
        fr = f_readdir(&dir, &fi);
        if ((fr != FR_OK) || (fi.fname[0] == 0))
        {
            break;
        }
        cnt++;
        const char *type = (fi.fattrib & AM_DIR) ? "<DIR>" : "     ";
        if (fi.fattrib & AM_DIR)
        {
            snprintf(buf, sizeof(buf), "%s %s/\r\n", type, fi.fname);
        }
        else
        {
            snprintf(buf, sizeof(buf), "%s %-16s %lu\r\n", type, fi.fname,
                     (unsigned long)fi.fsize);
        }
        cli_echo(buf);
    }
    f_closedir(&dir);
    snprintf(buf, sizeof(buf), "%u item(s)\r\n", (unsigned)cnt);
    cli_echo(buf);
}

static void cmd_mkdir(const char *path)
{
    cli_print_fr("mkdir", path, f_mkdir(path));
}

static void cmd_rm(const char *path)
{
    /* 目录删除：尝试 f_unlink（空目录可直接删，非空失败） */
    cli_print_fr("rm", path, f_unlink(path));
}

static void cmd_mv(const char *oldp, const char *newp)
{
    FRESULT fr = f_rename(oldp, newp);
    LOG_I("cli", "mv %s -> %s: %s (FR=%d)", oldp, newp,
          (fr == FR_OK) ? "OK" : "FAIL", (int)fr);
}

static void cmd_cat(const char *path)
{
    FIL f;
    UINT br = 0U;
    static uint8_t rbuf[64];

    FRESULT fr = f_open(&f, path, FA_READ);
    if (fr != FR_OK)
    {
        cli_print_fr("cat", path, fr);
        return;
    }

    for (;;)
    {
        fr = f_read(&f, rbuf, (UINT)sizeof(rbuf), &br);
        if ((fr != FR_OK) || (br == 0U))
        {
            break;
        }
        rbuf[br] = '\0';
        cli_echo((const char *)rbuf);
    }
    f_close(&f);
    cli_echo("\r\n");
}

static void cmd_write(const char *path, const char *text, uint8_t append_mode)
{
    FIL f;
    UINT bw = 0U;
    BYTE mode = append_mode ? (FA_OPEN_APPEND | FA_WRITE)
                            : (FA_CREATE_ALWAYS | FA_WRITE);

    FRESULT fr = f_open(&f, path, mode);
    if (fr != FR_OK)
    {
        cli_print_fr(append_mode ? "append" : "write", path, fr);
        return;
    }
    fr = f_write(&f, text, (UINT)strlen(text), &bw);
    if (fr == FR_OK)
    {
        /* 追加模式补换行 */
        if (append_mode)
        {
            static const char nl[] = "\r\n";
            (void)f_write(&f, nl, 2U, &bw);
        }
        LOG_I("cli", "%s %s: OK (%lu bytes)", append_mode ? "append" : "write",
              path, (unsigned long)bw);
    }
    else
    {
        cli_print_fr(append_mode ? "append" : "write", path, fr);
    }
    f_close(&f);
}

static void cmd_stat(const char *path)
{
    FILINFO fi;
    FRESULT fr = f_stat(path, &fi);
    if (fr != FR_OK)
    {
        cli_print_fr("stat", path, fr);
        return;
    }
    LOG_I("cli", "stat %s: %s size=%lu date=%u-%02u-%02u %02u:%02u",
          path, (fi.fattrib & AM_DIR) ? "<DIR>" : "file",
          (unsigned long)fi.fsize,
          (unsigned)((fi.fdate >> 9) + 1980U),
          (unsigned)((fi.fdate >> 5) & 0xFU),
          (unsigned)(fi.fdate & 0x1FU),
          (unsigned)(fi.ftime >> 11),
          (unsigned)((fi.ftime >> 5) & 0x3FU));
}

static void cmd_df(void)
{
    FATFS *pfs = NULL;
    DWORD free_clusters = 0U;
    FRESULT fr = f_getfree("", &free_clusters, &pfs);
    if ((fr != FR_OK) || (pfs == NULL))
    {
        LOG_E("cli", "df: f_getfree failed (FR=%d)", (int)fr);
        return;
    }
    uint64_t free_kb = (uint64_t)free_clusters * (uint64_t)pfs->csize / 2ULL;
    uint64_t total_kb = (uint64_t)(pfs->n_fatent - 2U) * (uint64_t)pfs->csize / 2ULL;
    LOG_I("cli", "df: free=%lu KB total=%lu KB (%u%% used)",
          (unsigned long)free_kb, (unsigned long)total_kb,
          (unsigned)((total_kb - free_kb) * 100ULL / total_kb));
}

/* ---- 行解析与分发 ---- */
static void cli_process_line(char *line)
{
    /* 分割：cmd arg1 arg2...（第 3 个参数起保留整段含空格，供 write/append 文本） */
    char *args[CLI_ARGS_MAX];
    uint8_t argc = 0U;
    char *p = line;

    while ((*p != '\0') && (argc < CLI_ARGS_MAX))
    {
        while ((*p == ' ') || (*p == '\t'))
        {
            p++;
        }
        if (*p == '\0')
        {
            break;
        }
        args[argc++] = p;
        if (argc >= 3U)
        {
            break;   /* 第 3 个参数 = 剩余全部（write/append 的多词文本） */
        }
        while ((*p != '\0') && (*p != ' ') && (*p != '\t'))
        {
            p++;
        }
        if (*p != '\0')
        {
            *p++ = '\0';
        }
    }

    if (argc == 0U)
    {
        return;
    }

    /* 回显命令行（终端体验） */
    LOG_I("cli", "$ %s", line);

    const char *cmd = args[0];
    if (strcmp(cmd, "help") == 0)
    {
        cmd_help();
    }
    else if (strcmp(cmd, "ls") == 0)
    {
        cmd_ls((argc > 1) ? args[1] : "/");
    }
    else if (strcmp(cmd, "mkdir") == 0)
    {
        if (argc > 1) { cmd_mkdir(args[1]); }
        else { LOG_W("cli", "usage: mkdir <dir>"); }
    }
    else if (strcmp(cmd, "rm") == 0)
    {
        if (argc > 1) { cmd_rm(args[1]); }
        else { LOG_W("cli", "usage: rm <file|dir>"); }
    }
    else if (strcmp(cmd, "mv") == 0)
    {
        if (argc > 2) { cmd_mv(args[1], args[2]); }
        else { LOG_W("cli", "usage: mv <old> <new>"); }
    }
    else if (strcmp(cmd, "cat") == 0)
    {
        if (argc > 1) { cmd_cat(args[1]); }
        else { LOG_W("cli", "usage: cat <file>"); }
    }
    else if (strcmp(cmd, "write") == 0)
    {
        if (argc > 2) { cmd_write(args[1], args[2], 0U); }
        else { LOG_W("cli", "usage: write <file> <text...>"); }
    }
    else if (strcmp(cmd, "append") == 0)
    {
        if (argc > 2) { cmd_write(args[1], args[2], 1U); }
        else { LOG_W("cli", "usage: append <file> <text...>"); }
    }
    else if (strcmp(cmd, "stat") == 0)
    {
        if (argc > 1) { cmd_stat(args[1]); }
        else { LOG_W("cli", "usage: stat <file>"); }
    }
    else if (strcmp(cmd, "df") == 0)
    {
        cmd_df();
    }
    else
    {
        LOG_W("cli", "unknown command '%s' (try help)", cmd);
    }
}

/* ---- 任务入口 ---- */
void SD_CLI_TaskEntry(void *arg)
{
    (void)arg;
    s_cli_task = xTaskGetCurrentTaskHandle();
    char line[CLI_LINE_MAX];
    uint16_t line_len = 0U;

    /* SD 初始化裕量：卡就绪前 CLI 不激活，绝不与 SD 初始化/RW 测试抢时序 */
    LOG_I("cli", "waiting for SD ready before activating...");
    while ((SYS_GetState() != SYS_STATE_SD_READY) &&
           (SYS_GetState() != SYS_STATE_FAULT))
    {
        vTaskDelay(pdMS_TO_TICKS(200U));
    }
    LOG_I("cli", "SD %s, CLI activated", SYS_StateText(SYS_GetState()));
    cli_prompt();

    for (;;)
    {
        /* 等字节入队通知或 100ms 超时（防通知丢失兜底） */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100U));

        /* 出队并逐字节处理：回显 + 行累积 + 退格 + 回车执行（全部在任务上下文） */
        while (s_rxq_cnt > 0U)
        {
            uint8_t c;
            taskENTER_CRITICAL();
            c = s_rxq[s_rxq_rd];
            s_rxq_rd = (s_rxq_rd + 1U) & (CLI_RXQ_SIZE - 1U);
            s_rxq_cnt--;
            taskEXIT_CRITICAL();

            if ((c == '\r') || (c == '\n'))
            {
                log_raw("\r\n", 2U);
                if (line_len > 0U)
                {
                    line[line_len] = '\0';
                    cli_process_line(line);
                    line_len = 0U;
                }
                cli_prompt();   /* 空回车也重新显示提示符 */
            }
            else if ((c == 0x08U) || (c == 0x7FU))
            {
                if (line_len > 0U)
                {
                    line_len--;
                    log_raw("\b \b", 3U);
                }
            }
            else if (line_len < (CLI_LINE_MAX - 1U))
            {
                line[line_len++] = (char)c;
                log_raw((const char *)&c, 1U);   /* 字符回显 */
            }
        }
    }
}

/* ---- 初始化 ---- */
void SD_CLI_Init(void)
{
    uart10.module_callback = cli_rx_callback;
    LOG_I("cli", "SD CLI ready (type 'help' for commands)");
}
