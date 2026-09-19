/*
 * cmd_parse.h - 串口命令行解析（**纯逻辑**：无 HAL/FreeRTOS/stdlib 依赖 → 宿主机可穷举测试）
 *
 * 协议（docs/协议_串口控制_v1.md §2）：
 *   上位机 → 板:  #<CMD> [arg ...]      行尾 \r\n 或 \n
 *   本解析器只认 '#' 开头的行；参数分两类：
 *     · 数值参数 → f[]（十进制，可带正负号与小数点，不支持指数）
 *     · 关键字参数 → w[][]（自动转大写，如 P/V/T/POS/ONCE/KD_DAMP）
 *   解析结果只给"命令码 + 参数"，语义/回包由调用方（CmdRx 任务）负责。
 *
 * 约定：
 *   · 命令名与关键字大小写不敏感（内部统一转大写比较）
 *   · 数值参数上限 CMD_MAX_FARGS 个、关键字参数上限 CMD_MAX_WARGS 个，超限 → CMD_PARSE_BADARG
 *   · 任意非法数值（"1.2.3"、"12a"、空）→ CMD_PARSE_BADARG
 */
#ifndef CMD_PARSE_H
#define CMD_PARSE_H

#include "stdint.h"

#define CMD_MAX_FARGS (6u)  /* #SETP 需要 5 个数（p v kp kd t） */
#define CMD_MAX_WARGS (2u)
#define CMD_WARG_LEN (10u)  /* 关键字最长 9 字符（含终止符） */

typedef enum
{
    CMD_PARSE_NOTCMD = 0, /* 不是命令（不以 '#' 开头）→ 调用方静默忽略 */
    CMD_PARSE_OK,
    CMD_PARSE_UNKNOWN, /* 未知命令 → @ERR 1 */
    CMD_PARSE_BADARG   /* 参数越界/格式错 → @ERR 2 */
} Cmd_ParseRes_e;

typedef enum
{
    CMD_NONE = 0,
    CMD_PING,
    CMD_VER,
    CMD_STAT,
    CMD_LOG,
    CMD_CLR,
    CMD_SET,
    CMD_GET,
    CMD_EN,
    CMD_DIS,
    CMD_STOP,
    CMD_ESTOP,
    CMD_ZERO,
    CMD_MODE,
    CMD_DAMP,
    CMD_V,
    CMD_P,
    CMD_T,
    CMD_IMP,
    CMD_HOLD,
    CMD_SETP,
    CMD_LIM,
    CMD_RATE,
    CMD_WD,
    CMD_TEL,
    CMD_HELP,
    CMD_ST,   /* 模块自检汇总（分块调试入口；见 docs/操作手册_串口命令.md） */
    CMD__COUNT
} Cmd_Id_e;

typedef struct
{
    Cmd_Id_e id;
    uint8_t nf;                         /* 数值参数个数 */
    float f[CMD_MAX_FARGS];             /* 数值参数 */
    uint8_t nw;                         /* 关键字参数个数 */
    char w[CMD_MAX_WARGS][CMD_WARG_LEN];/* 关键字参数（已转大写） */
} Cmd_t;

/* 命令名 → 命令码（大小写不敏感；未知返回 CMD_NONE） */
static inline Cmd_Id_e cmd_lookup(const char *name, uint8_t len)
{
    static const char *const names[CMD__COUNT] = {
        "",     "PING", "VER", "STAT", "LOG",  "CLR",  "SET",  "GET", "EN",
        "DIS",  "STOP", "ESTOP", "ZERO", "MODE", "DAMP", "V",    "P",   "T",
        "IMP",  "HOLD", "SETP", "LIM",  "RATE", "WD",   "TEL",  "HELP", "ST"};
    uint8_t i;
    uint8_t k;

    if ((name == NULL) || (len == 0u) || (len > 12u))
        return CMD_NONE;

    for (i = 1u; i < (uint8_t)CMD__COUNT; i++)
    {
        const char *n = names[i];
        uint8_t m = 0u;
        uint8_t ok = 1u;

        while (n[m] != '\0')
            m++;
        if (m != len)
            continue;
        for (k = 0u; k < len; k++)
        {
            char a = name[k];
            if ((a >= 'a') && (a <= 'z'))
                a = (char)(a - 32);
            if (a != n[k])
            {
                ok = 0u;
                break;
            }
        }
        if (ok != 0u)
            return (Cmd_Id_e)i;
    }
    return CMD_NONE;
}

/* 解析一个十进制浮点数（返回 1=成功）。不支持指数；最多一个小数点 */
static inline uint8_t cmd_atof(const char *s, uint8_t len, float *out)
{
    uint8_t i = 0u;
    uint8_t seen_dot = 0u;
    uint8_t seen_digit = 0u;
    uint8_t neg = 0u;
    float v = 0.0f;
    float scale = 0.1f;

    if ((s == NULL) || (out == NULL) || (len == 0u))
        return 0u;
    if ((s[0] == '-') || (s[0] == '+'))
    {
        neg = (s[0] == '-') ? 1u : 0u;
        i = 1u;
    }
    for (; i < len; i++)
    {
        char c = s[i];
        if ((c >= '0') && (c <= '9'))
        {
            seen_digit = 1u;
            if (seen_dot == 0u)
            {
                v = v * 10.0f + (float)(c - '0');
            }
            else
            {
                v += (float)(c - '0') * scale;
                scale *= 0.1f;
            }
        }
        else if (c == '.')
        {
            if (seen_dot != 0u)
                return 0u; /* 多个小数点 */
            seen_dot = 1u;
        }
        else
        {
            return 0u; /* 非法字符（含指数 e） */
        }
    }
    if (seen_digit == 0u)
        return 0u;
    *out = (neg != 0u) ? -v : v;
    return 1u;
}

/* 解析整行。返回 Cmd_ParseRes_e；cmd 仅在返回 OK 时有效 */
static inline Cmd_ParseRes_e Cmd_ParseLine(const char *line, uint16_t len, Cmd_t *cmd)
{
    uint16_t i = 0u;
    uint16_t start;
    uint8_t wlen;
    char name[16];

    if ((line == NULL) || (cmd == NULL))
        return CMD_PARSE_NOTCMD;
    cmd->id = CMD_NONE;
    cmd->nf = 0u;
    cmd->nw = 0u;

    /* 跳过前导空白 */
    while ((i < len) && ((line[i] == ' ') || (line[i] == '\t')))
        i++;
    if (i >= len)
        return CMD_PARSE_NOTCMD;
    if (line[i] != '#')
        return CMD_PARSE_NOTCMD;
    i++;

    /* 命令名 */
    start = i;
    while ((i < len) && (line[i] != ' ') && (line[i] != '\t') && (line[i] != '\r') && (line[i] != '\n'))
        i++;
    wlen = (uint8_t)(i - start);
    if ((wlen == 0u) || (wlen > 12u))
        return CMD_PARSE_BADARG;
    {
        uint8_t k;
        for (k = 0u; k < wlen; k++)
            name[k] = line[start + k];
        name[wlen] = '\0';
    }
    cmd->id = cmd_lookup(name, wlen);
    if (cmd->id == CMD_NONE)
        return CMD_PARSE_UNKNOWN;

    /* 参数 */
    while (i < len)
    {
        /* 跳过分隔符 */
        while ((i < len) && ((line[i] == ' ') || (line[i] == '\t')))
            i++;
        if ((i >= len) || (line[i] == '\r') || (line[i] == '\n'))
            break;

        start = i;
        while ((i < len) && (line[i] != ' ') && (line[i] != '\t') && (line[i] != '\r') && (line[i] != '\n'))
            i++;

        {
            uint8_t tlen = (uint8_t)(i - start);
            float v;
            if (tlen == 0u)
                continue;
            if (tlen > 16u)
                return CMD_PARSE_BADARG;
            if (cmd_atof(&line[start], tlen, &v) != 0u)
            {
                if (cmd->nf >= (uint8_t)CMD_MAX_FARGS)
                    return CMD_PARSE_BADARG;
                cmd->f[cmd->nf] = v;
                cmd->nf++;
            }
            else
            {
                uint8_t k;
                char c0 = line[start];

                /* 【宿主测试抓到的坑】"数字形状但解析失败"（如 1.2.3 / 1e3 / -）不能当关键字静默吞掉，
                 * 必须明确报 @ERR 2 —— 关键字一律以字母/下划线开头。 */
                if (((c0 >= '0') && (c0 <= '9')) || (c0 == '-') || (c0 == '+') || (c0 == '.'))
                {
                    return CMD_PARSE_BADARG;
                }
                if (cmd->nw >= (uint8_t)CMD_MAX_WARGS)
                    return CMD_PARSE_BADARG;
                for (k = 0u; (k < tlen) && (k < (uint8_t)(CMD_WARG_LEN - 1u)); k++)
                {
                    char c = line[start + k];
                    if ((c >= 'a') && (c <= 'z'))
                        c = (char)(c - 32);
                    cmd->w[cmd->nw][k] = c;
                }
                cmd->w[cmd->nw][k] = '\0';
                cmd->nw++;
            }
        }
    }
    return CMD_PARSE_OK;
}

/* 关键字比较（w 已大写；need 需为大写字面量），大小写不敏感 */
static inline uint8_t cmd_word_is(const Cmd_t *cmd, uint8_t idx, const char *need)
{
    uint8_t i = 0u;

    if ((cmd == NULL) || (idx >= cmd->nw) || (need == NULL))
        return 0u;
    while (need[i] != '\0')
    {
        char a = cmd->w[idx][i];
        char b = need[i];
        if ((b >= 'a') && (b <= 'z'))
            b = (char)(b - 32);
        if ((a == '\0') || (a != b))
            return 0u;
        i++;
    }
    return (cmd->w[idx][i] == '\0') ? 1u : 0u;
}

#endif /* CMD_PARSE_H */
