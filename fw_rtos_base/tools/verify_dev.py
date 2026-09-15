#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
tools/verify_dev.py - 开发期一键核验（烧录前的门禁）

用法:
    python tools/verify_dev.py            # 全量：宿主机逻辑测试 + 静态断言 + UV4 构建 + 链接证据
    python tools/verify_dev.py --fast     # 跳过 UV4 构建（只跑宿主机测试 + 静态断言）

覆盖的失败模式（都是本项目实际踩过的）:
    1. 任务 Init 挂在被注释的 main_cpp() 里 -> 任务永不创建（日志里整块消失）
    2. xTaskCreate 静默失败（heap 32KB 紧张）
    3. 日志写了中文 -> 串口终端乱码
    4. 协议/事件机逻辑错误（宿主机 gcc 直接跑真实源码，对照厂商文档示例帧）
    5. 新文件没进 uvprojx / 没进镜像（链接 MAP 交叉引用核对）

退出码: 0=全部通过；非 0=存在失败（可作 CI/交付门禁）
"""
import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tempfile

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

BASE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MDK = os.path.join(BASE, "MDK-ARM")
PROJ = "F427IIH6_CAN"
UV4 = r"E:\keil_arm\AppData\Local\Keil_v5\UV4\UV4.exe"
GCC = r"C:\msys64\mingw64\bin\gcc.exe"

RESULTS = []


def check(name, ok, detail=""):
    RESULTS.append((name, bool(ok)))
    print(("  PASS  " if ok else "  FAIL  ") + name + (f"   | {detail}" if detail and not ok else ""))


def read(p):
    return open(p, encoding="utf-8", errors="replace").read()


def run(cmd, cwd=None):
    return subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, encoding="utf-8", errors="replace")


# ============================================================ 宿主机逻辑测试
KEY_TEST = r"""
#include "stdio.h"
#include "key_core.h"
static int F, N; static KeyCore_Msg_t E[64];
static void t(KeyCore_t *k, uint8_t lv, int n){int i;for(i=0;i<n;i++){KeyCore_Msg_t m;if(KeyCore_Step(k,lv,&m)&&N<64)E[N++]=m;}}
static void rst(void){N=0;}
static int c(KeyCore_Event_e e){int i,n=0;for(i=0;i<N;i++)if(E[i].evt==e)n++;return n;}
static KeyCore_Msg_t f(KeyCore_Event_e e){int i;KeyCore_Msg_t z;z.evt=KEYC_EVT_NONE;z.hold_ms=0;z.gap_ms=0;z.clicks=0;
  for(i=0;i<N;i++)if(E[i].evt==e)return E[i];return z;}
#define CK(c,m) do{ if(c) printf("  PASS  %s\n",m); else { printf("  FAIL  %s\n",m); F++; } }while(0)
int main(void){
  KeyCore_t k; KeyCore_Msg_t m;
  KeyCore_Init(&k,0,10); t(&k,1,50); rst();
  t(&k,0,11); t(&k,1,5); t(&k,1,40);
  CK(c(KEYC_EVT_CLICK)==1 && c(KEYC_EVT_DOUBLE)==0, "A1 single short press -> CLICK only");
  m=f(KEYC_EVT_CLICK); CK(m.clicks==1, "A1 clicks=1"); CK(m.hold_ms>=100&&m.hold_ms<=120, "A1 hold~110ms");
  rst(); t(&k,0,11); t(&k,1,15); t(&k,0,11); t(&k,1,5); t(&k,1,40);
  CK(c(KEYC_EVT_DOUBLE)==1 && c(KEYC_EVT_CLICK)==0, "A2 fast double press -> DOUBLE only");
  CK(f(KEYC_EVT_DOUBLE).clicks==2, "A2 clicks=2");
  rst(); t(&k,0,11); t(&k,1,5); t(&k,1,60); t(&k,0,11); t(&k,1,5); t(&k,1,40);
  CK(c(KEYC_EVT_CLICK)==2 && c(KEYC_EVT_DOUBLE)==0, "A3 600ms apart -> 2x CLICK");
  rst(); t(&k,1,20); t(&k,0,100);
  CK(k.hold_ms>=950&&k.hold_ms<=1010, "B1 hold progress ~1000ms");
  t(&k,0,103); t(&k,1,5); CK(c(KEYC_EVT_HOLD_RELEASE)==1, "B1 long press -> HOLD_RELEASE");
  m=f(KEYC_EVT_HOLD_RELEASE); CK(m.hold_ms>=2000&&m.hold_ms<=2060, "B1 hold_ms>=2000 (2s row threshold)");
  t(&k,1,40); CK(c(KEYC_EVT_CLICK)==0, "B1 no CLICK after hold");
  rst(); t(&k,0,3); t(&k,1,40); CK(N==0, "C1 30ms bounce ignored");
  rst(); t(&k,0,1010); CK(c(KEYC_EVT_STUCK)==1, "C2 10.1s -> STUCK");
  t(&k,1,40); CK(c(KEYC_EVT_CLICK)==0&&c(KEYC_EVT_HOLD_RELEASE)==0, "C2 stuck press invalidated (no action)");
  KeyCore_Init(&k,1,10); rst(); t(&k,0,50); t(&k,1,11); t(&k,0,5); t(&k,0,40);
  CK(c(KEYC_EVT_CLICK)==1, "C3 active_level=1 works");
  KeyCore_Init(&k,0,10); rst(); t(&k,1,20); t(&k,0,11); t(&k,1,40); t(&k,0,11); t(&k,1,5); t(&k,1,40);
  CK(c(KEYC_EVT_CLICK)==2&&c(KEYC_EVT_DOUBLE)==0, "D1 gap 400ms -> 2x CLICK");
  rst(); t(&k,1,20); t(&k,0,11); t(&k,1,6); t(&k,0,11); t(&k,1,5); t(&k,1,40);
  CK(c(KEYC_EVT_DOUBLE)==0, "D2 gap 60ms -> not DOUBLE");
  printf("  ==> %s (fail=%d)\n", F?"FAIL":"ALL PASS", F); return F?1:0;
}
"""

MOTOR_TEST = r"""
#include "stdio.h"
#include "string.h"
#include "math.h"
#include "FreeRTOS.h"
#include "task.h"
#include "motor_8108.h"
static CAN_HandleTypeDef h; static CANInstance inst; static uint32_t cap_id; static uint8_t cap[8];
CAN_HandleTypeDef hcan1; /* 与工程 can.h 里的 extern 对应 */
TickType_t xTaskGetTickCountFromISR(void){return 1234u;}
uint32_t HAL_CAN_GetTxMailboxesFreeLevel(CAN_HandleTypeDef *x){(void)x;return 3U;}
static uint32_t g_hal_err;
uint32_t HAL_CAN_GetError(CAN_HandleTypeDef *x){(void)x;return g_hal_err;}
TickType_t xTaskGetTickCount(void){return 1234u;}
uint32_t HAL_CAN_AddTxMessage(CAN_HandleTypeDef *x, CAN_TxHeaderTypeDef *hdr, uint8_t *d, uint32_t *mb){
  (void)x;(void)mb;cap_id=hdr->StdId;memcpy(cap,d,8);return HAL_OK;}
CANInstance *CANRegister(CAN_Init_Config_s *cfg){memset(&inst,0,sizeof(inst));inst.can_handle=cfg->can_handle;return &inst;}
static int F;
#define CK(c,m) do{ if(c) printf("  PASS  %s\n",m); else { printf("  FAIL  %s\n",m); F++; } }while(0)
static float d16(uint16_t r,float a,float b){return a+(float)r/65535.0f*(b-a);}
static float d12(uint16_t r,float a,float b){return a+(float)r/4095.0f*(b-a);}
int main(void){
  J8108_t *dev; uint16_t p,v,kd,tx0;
  const uint8_t en[8]={0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFC};
  const uint8_t fb[8]={0x01,0x7F,0xA6,0x7F,0xF8,0x16,0x4B,0x44};
  J8108_Init(&h); dev=J8108_Get();
  J8108_SendCmd(J8108_CMD_ENABLE);
  CK(cap_id==0x01, "ENABLE frame ID=0x01"); CK(memcmp(cap,en,8)==0, "ENABLE data FF..FC");
  J8108_SendCmd(J8108_CMD_DISABLE); CK(cap[7]==0xFD, "DISABLE tail FD");
  J8108_SendCmd(J8108_CMD_ZERO);    CK(cap[7]==0xFE, "ZERO tail FE");
  J8108_SendCmd(J8108_CMD_REBOOT);  CK(cap[7]==0xFB, "REBOOT tail FB");
  tx0=dev->fb.tx_cnt; J8108_SendMIT(0.0f,10.0f,0.0f,1.0f,0.0f);
  CK(cap_id==0x201, "MIT frame ID=0x201"); CK(dev->fb.tx_cnt==tx0+1u, "tx_cnt++");
  p=(uint16_t)((cap[0]<<8)|cap[1]); v=(uint16_t)((cap[2]<<4)|(cap[3]>>4)); kd=(uint16_t)((cap[5]<<4)|(cap[6]>>4));
  CK(cap[2]==0x9C, "v=10 high byte 0x9C (doc)"); CK(kd==0x333, "Kd=1 = 0x333 (doc)");
  CK(fabsf(d16(p,-25.12f,25.12f))<0.002f, "decode p ~ 0"); CK(fabsf(d12(v,-45.0f,45.0f)-10.0f)<0.05f, "decode v ~ 10");
  memcpy(dev->fb.raw,fb,8); J8108_Update();
  CK(fabsf(dev->fb.pos+0.0678f)<0.005f, "fb POS ~ -0.069rad (doc)");
  CK(fabsf(dev->fb.torque-0.198f)<0.01f, "fb T ~ 0.198Nm (doc)");
  CK(fabsf(dev->fb.t_mos-29.41f)<0.2f, "fb TMos ~ 29.41C (doc)");
  CK(fabsf(dev->fb.t_rotor-26.67f)<0.2f, "fb TMotor ~ 26.67C (doc)");
  J8108_SendMIT(0.0f,100.0f,0.0f,0.0f,0.0f); v=(uint16_t)((cap[2]<<4)|(cap[3]>>4));
  CK(v==0x0FFF, "v=100 clamped to 0xFFF");
  /* ---- CAN 状态机（屏上四态的数据源：INIT FAIL / INIT OK / READY / BUS ERR）---- */
  J8108_Init(&h); dev=J8108_Get(); J8108_Update();
  CK(dev->init_ok==1u, "status: init_ok=1 after CANRegister");
  CK(dev->status==J8108_ST_INIT_OK, "status: INIT_OK when no frame on bus");
  dev->fb.rx_count=1u; dev->fb.last_rx_ms=1234u; J8108_Update();
  CK(dev->status==J8108_ST_READY, "status: READY when feedback fresh (<200ms)");
  dev->fb.last_rx_ms=0u; J8108_Update();
  CK(dev->status==J8108_ST_INIT_OK, "status: back to INIT_OK when feedback stale (link lost)");
  g_hal_err=0x8u; J8108_Update(); CK(dev->status==J8108_ST_BUS_ERR, "status: BUS_ERR on HAL error");
  g_hal_err=0u; dev->init_ok=0u; J8108_Update(); CK(dev->status==J8108_ST_INIT_FAIL, "status: INIT_FAIL when register failed");
  CK(strcmp(J8108_StatusStr(J8108_ST_READY),"READY")==0, "status: label string");
  /* ---- M2 数据快照：帧一致只读视图（显示/日志用）---- */
  { J8108_Snapshot_t s1, s2;
    J8108_Init(&h); dev=J8108_Get();
    memcpy(dev->fb.raw, fb, 8); dev->fb.rx_count=1u; dev->fb.last_rx_ms=1234u; dev->new_frame=1u;
    J8108_Update();
    CK(dev->new_frame==0u, "snapshot: new_frame consumed under critical section");
    J8108_CopySnapshot(&s1);
    CK(s1.valid==1u && s1.seq>0u && ((s1.seq%2u)==0u), "snapshot: valid=1 & even seq");
    CK(memcmp(s1.raw,fb,8)==0, "snapshot: raw[8] belongs to the SAME frame as decoded values");
    CK(fabsf(s1.pos-dev->fb.pos)<1e-6f, "snapshot: pos mirrors driver");
    CK(fabsf(s1.pos_out_deg-(dev->fb.pos/8.0f*57.29578f))<0.01f, "snapshot: output deg = pos/8 (gear 8:1)");
    CK(fabsf(s1.vel_rpm-(dev->fb.vel*9.54930f))<0.01f, "snapshot: RPM conversion");
    CK(s1.status==dev->status && s1.rx_count==dev->fb.rx_count, "snapshot: status & counters mirrored");
    J8108_CopySnapshot(&s2);
    CK(s2.seq==s1.seq, "snapshot: seq stable across reads when no writer");
    J8108_Update(); /* 再发布一次 */
    J8108_CopySnapshot(&s2);
    CK(s2.seq>s1.seq, "snapshot: seq advances on each publish");
  }
  printf("  ==> %s (fail=%d)\n", F?"FAIL":"ALL PASS", F); return F?1:0;
}
"""

STUBS = {
    "FreeRTOS.h": '#ifndef FR_STUB\n#define FR_STUB\n#include "stdint.h"\ntypedef uint32_t TickType_t;\n#define portTICK_PERIOD_MS 1\n'
                  '#define taskENTER_CRITICAL() do { } while (0)\n#define taskEXIT_CRITICAL() do { } while (0)\n#endif\n',
    "task.h": '#ifndef TK_STUB\n#define TK_STUB\n#include "FreeRTOS.h"\nTickType_t xTaskGetTickCountFromISR(void);\nTickType_t xTaskGetTickCount(void);\n#endif\n',
    "can.h": '#ifndef CAN_STUB\n#define CAN_STUB\n#include "stdint.h"\n#define DISABLE 0U\n#define HAL_OK 0U\n#define CAN_ID_STD 0U\n#define CAN_RTR_DATA 0U\n'
             'typedef struct { uint32_t _d; } CAN_HandleTypeDef;\n'
             'extern CAN_HandleTypeDef hcan1;\n'
             'typedef struct { uint32_t StdId, ExtId, IDE, RTR, DLC, TransmitGlobalTime; } CAN_TxHeaderTypeDef;\n'
             'uint32_t HAL_CAN_GetTxMailboxesFreeLevel(CAN_HandleTypeDef *h);\n'
             'uint32_t HAL_CAN_GetError(CAN_HandleTypeDef *h);\n'
             'uint32_t HAL_CAN_AddTxMessage(CAN_HandleTypeDef *h, CAN_TxHeaderTypeDef *hdr, uint8_t *data, uint32_t *mailbox);\n#endif\n',
    "bsp_log.h": '#ifndef BL_STUB\n#define BL_STUB\n#define LOG_D(tag, ...) do { } while (0)\n#define LOG_I(tag, ...) do { } while (0)\n#define LOG_W(tag, ...) do { } while (0)\n#define LOG_E(tag, ...) do { } while (0)\n#endif\n',
    "bsp_can.h": '#ifndef BC_STUB\n#define BC_STUB\n#include "stdint.h"\n#include "can.h"\n'
                 'typedef struct _i { CAN_HandleTypeDef *can_handle; uint8_t rx_buff[8]; uint8_t rx_len;\n'
                 '  void (*can_module_callback)(struct _i *); void *id; } CANInstance;\n'
                 'typedef struct { CAN_HandleTypeDef *can_handle; uint32_t tx_id, rx_id;\n'
                 '  void (*can_module_callback)(CANInstance *); void *id; } CAN_Init_Config_s;\n'
                 'CANInstance *CANRegister(CAN_Init_Config_s *config);\n#endif\n',
}

# ---- M3 动作策略层（j8108_action.h 纯逻辑头）宿主机测试 ----
ACTION_TEST = r"""
#include "stdio.h"
#include "string.h"
#include "j8108_action.h"
static int F;
#define CK(c, m) do { if (c) printf("  PASS  %s\n", m); else { printf("  FAIL  %s\n", m); F++; } } while (0)
int main(void)
{
    int r, viol = 0;

    CK(J8108_ActionDecide(1, 5000, 1, ACT_ST_READY) == ACT_DO_NOTHING, "read-only row1 -> DO_NOTHING (silent)");
    CK(J8108_ActionDecide(7, 5000, 1, ACT_ST_INIT_OK) == ACT_DO_NOTHING, "read-only row7 -> DO_NOTHING");
    CK(J8108_ActionDecide(0, 1999, 1, ACT_ST_INIT_OK) == ACT_REJECT_SHORT, "1999ms < 2.0s -> REJECT_SHORT");
    CK(J8108_ActionDecide(0, 2000, 1, ACT_ST_INIT_OK) == ACT_ENABLE, "exactly 2.0s -> triggered (enable)");
    CK(J8108_ActionDecide(0, 2001, 1, ACT_ST_INIT_OK) == ACT_ENABLE, ">2.0s -> triggered");
    CK(J8108_ActionDecide(0, 3000, 0, ACT_ST_INIT_OK) == ACT_REJECT_COOLDOWN, "cooldown active -> rejected");
    CK(J8108_ActionDecide(0, 3000, 1, ACT_ST_INIT_FAIL) == ACT_REJECT_CAN, "CAN init fail -> rejected");
    CK(J8108_ActionDecide(0, 3000, 1, ACT_ST_BUS_ERR) == ACT_REJECT_CAN, "CAN bus err -> rejected");
    CK(J8108_ActionDecide(0, 3000, 1, ACT_ST_INIT_OK) == ACT_ENABLE, "no feedback yet -> ENABLE");
    CK(J8108_ActionDecide(0, 3000, 1, ACT_ST_READY) == ACT_DISABLE, "feedback flowing -> DISABLE (toggle)");
    for (r = ACTR_NONE; r <= ACTR_CAN_NOT_READY; r++)
    {
        const char *s = J8108_ActionResultStr((J8108_ActionResult_e)r);
        if (strlen(s) > 21u) { printf("  FAIL  result string >21 chars: %s\n", s); viol++; }
    }
    CK(viol == 0, "all action-result strings fit the 21-char row");
    CK(strcmp(J8108_ActionResultStr(ACTR_EN_NO_ACK), "EN NO ACK/NODE") == 0, "result string mapping");
    CK(J8108_ACTION_HOLD_MS == 2000u && J8108_ACTION_COOLDOWN_MS == 1500u && J8108_ACTION_ROW == 0u,
       "constants: 2s hold / 1.5s cooldown / row0");
    printf("  ==> %s (fail=%d)\n", F ? "FAIL" : "ALL PASS", F);
    return F ? 1 : 0;
}
"""


def host_tests(tmp):
    print("---- 宿主机逻辑测试（gcc 跑工程内真实源码）----")
    if not os.path.exists(GCC):
        check("gcc 可用（宿主机测试）", False, GCC)
        return
    cases = [
        ("按键事件机 key_core", "key", ["mcu_bsp/key/key_core.c", "mcu_bsp/key/key_core.h"], KEY_TEST),
        ("电机协议 motor_8108", "motor", ["mcu_bsp/Motor/motor_8108.c", "mcu_bsp/Motor/motor_8108.h"], MOTOR_TEST),
        ("单键动作策略 j8108_action", "action", ["mcu_bsp/Motor/j8108_action.h"], ACTION_TEST),
    ]
    for title, tag, srcs, harness in cases:
        d = os.path.join(tmp, tag)
        os.makedirs(d, exist_ok=True)
        for s in srcs:
            shutil.copy2(os.path.join(BASE, s.replace("/", os.sep)), os.path.join(d, os.path.basename(s)))
        if tag == "motor":
            for fn, body in STUBS.items():
                open(os.path.join(d, fn), "w", encoding="utf-8").write(body)
        compiles = [os.path.basename(s) for s in srcs if s.endswith(".c")] + ["harness.c"]
        open(os.path.join(d, "harness.c"), "w", encoding="utf-8").write(harness)
        exe = "t.exe"
        r = run([GCC, "-std=c99", "-Wall", "-Wextra", "-O1"] + compiles + ["-o", exe, "-lm"], cwd=d)
        if r.returncode != 0:
            check(f"{title}: gcc 编译", False, (r.stderr or "")[-200:])
            continue
        check(f"{title}: gcc 编译", True)
        run_r = run([os.path.join(d, exe)], cwd=d)
        print(run_r.stdout.rstrip())
        check(f"{title}: 全部断言通过", run_r.returncode == 0)


# ============================================================ 静态断言
def static_checks():
    print("---- 静态断言（宏 / 任务接线 / 日志 ASCII / 创建失败可见性）----")
    fc = read(os.path.join(BASE, "Task", "Inc", "feature_config.h"))
    macros = dict(re.findall(r"#define\s+(FEATURE_\w+)\s+(\d+)u", fc))
    check("feature_config.h: 8 个功能宏齐全", len(macros) == 8, str(sorted(macros)))

    mainc = read(os.path.join(BASE, "Core", "Src", "main.c"))
    live = [ln for ln in mainc.splitlines() if not ln.strip().startswith(("//", "*"))]

    def live_has(needle):
        return any(needle in ln for ln in live)

    check("main.c: Key_Task_Init() 在非注释代码中", live_has("Key_Task_Init();"))
    check("main.c: J8108_Task_Init() 在非注释代码中", live_has("J8108_Task_Init();"))
    check("main.c: 二者均在 #if FEATURE_* 保护块内", "#if FEATURE_KEY" in mainc and "#if FEATURE_J8108" in mainc)
    check("main.c: SD/CLI/小车 已被宏包住", all(f"#if FEATURE_{x}" in mainc for x in ("SD_CARD", "SD_CLI", "CAR_TASKS")))

    bad = []
    for rel in ("mcu_bsp/key/bsp_key.c", "Task/Src/J8108_Task.c", "Task/Src/Oled_Task.c", "mcu_bsp/Motor/motor_8108.c"):
        for i, ln in enumerate(read(os.path.join(BASE, rel.replace("/", os.sep))).splitlines(), 1):
            if re.search(r"\b(LOG_[DIWE]|printf)\s*\(", ln):
                for lit in re.findall(r'"([^"]*)"', ln):
                    if any(ord(c) > 127 for c in lit):
                        bad.append(f"{rel}:{i}")
    check("日志字符串全 ASCII（英文约定，防乱码）", not bad, "; ".join(bad[:3]))

    for rel in ("mcu_bsp/key/bsp_key.c", "Task/Src/J8108_Task.c", "Task/Src/Oled_Task.c"):
        t = read(os.path.join(BASE, rel.replace("/", os.sep)))
        n = t.count("xTaskCreate(")
        check(f"{os.path.basename(rel)}: xTaskCreate 均有 pdPASS 检查", n > 0 and t.count("pdPASS") >= n)

    # ---- 需求回归（用户 2026-09-15 明确要求，防止以后被改回去）----
    oled = read(os.path.join(BASE, "Task", "Src", "Oled_Task.c"))
    check("需求: 行0 覆盖 CAN 四态上屏（INIT FAIL/INIT OK/READY/BUS ERR）",
          all(f"J8108_ST_{s}" in oled for s in ("INIT_FAIL", "INIT_OK", "READY", "BUS_ERR")))
    check("需求: 长按计时条不上屏（bar[] 绘制已移除）", "bar[" not in oled)
    bkey = read(os.path.join(BASE, "mcu_bsp", "key", "bsp_key.h"))
    check("需求: PB2 高有效（KEY_ACTIVE_LEVEL=1u，A 板实测）",
          re.search(r"#define\s+KEY_ACTIVE_LEVEL\s+\(1u\)", bkey) is not None)

    # ---- M3 动作层接线（长按→发帧 + 保护层必须都在）----
    jt = read(os.path.join(BASE, "Task", "Src", "J8108_Task.c"))
    check("M3: 消费 HOLD_RELEASE 长按事件", "KEY_BIT_HOLD_RELEASE" in jt)
    check("M3: 走纯策略层 J8108_ActionDecide()", "J8108_ActionDecide(" in jt)
    check("M3: 真正发帧（J8108_SendCmd）", "J8108_SendCmd(" in jt)
    check("M3: L3.5 无 ACK 主动丢帧（防无限重传→总线错误）", "HAL_CAN_AbortTxRequest" in jt)
    check("M3: L4 反馈确认（使能→出现 / 失能→停止）", "ACTR_EN_OK" in jt and "ACTR_DIS_OK" in jt)
    check("M3: UI 发布光标行", "Oled_UiGetCursorRow" in oled and "s_cursor_row" in oled)
    check("M3: 行7 显示动作结果", "J8108_ActionResultStr" in oled)
    check("M2: 帧一致取帧（临界区 + new_frame）", "taskENTER_CRITICAL" in read(os.path.join(BASE, "mcu_bsp", "Motor", "motor_8108.c")))

    # ---- M4-a：HOLD 阻尼保持（= 让电机回传反馈的"心跳"通道）----
    check("M4-a: HOLD 模式开关与进入/退出", all(k in jt for k in ("J8108_HOLD_ON_ENABLE", "hold_enter", "hold_exit")))
    check("M4-a: HOLD 轮询（周期发 MIT + 看门狗）", "j8108_hold_poll" in jt and "J8108_HOLD_WD_MS" in jt)
    check("M4-a: 看门狗无 ACK/反馈丢失即停发（防无限重传）", "HOLD watchdog" in jt)
    check("M4-a: 屏上区分 HOLD/SEND", "J8108_IsHoldMode" in oled)
    check("M4-a: TX 无 ACK 判定带迟滞（j8108_tx_stuck：连续占用才判死）", "j8108_tx_stuck" in jt)
    check("M4-a: L3.5 单次检查（s_ack_checked）防反复重进 HOLD", "s_ack_checked" in jt)
    check("M4-a: 自动恢复（hold_want + 退避 retry + 失败上限）",
          all(k in jt for k in ("s_hold_want", "s_hold_retry_ms", "J8108_HOLD_RETRY_MAX")))
    check("M4-a: 反馈流日志只在数据新鲜时打印", "J8108_FB_TIMEOUT_MS) &&" in jt)
    check("需求: 10s 摘要链路断时不打印陈旧数值（只报 link DOWN）", "stale values not printed on purpose" in jt)


# ============================================================ 构建 + 链接证据
def build_and_link():
    print("---- 固件全量重建（UV4 -r）+ 链接证据 ----")
    if not os.path.exists(UV4):
        check("UV4 可用", False, UV4)
        return
    r = run([UV4, "-r", f"{PROJ}.uvprojx", "-j0", "-t", PROJ, "-o", "rebuild_verify.log"], cwd=MDK)
    log = read(os.path.join(MDK, "rebuild_verify.log"))
    check("UV4 退出码 0", r.returncode == 0, str(r.returncode))
    check("0 Error(s), 0 Warning(s)", "0 Error(s), 0 Warning(s)" in log)
    need = ["main.c", "motor_8108.c", "bsp_key.c", "key_core.c", "Oled_Task.c", "J8108_Task.c"]
    missing = [f for f in need if f"compiling {f}" not in log]
    check("6 个关键文件全部编译", not missing, str(missing))
    for l in log.splitlines():
        if "Program Size" in l:
            print("        " + l.strip())
    hexf = os.path.join(MDK, PROJ, f"{PROJ}.hex")
    print(f"        hex: {os.path.getsize(hexf)}B  sha256={hashlib.sha256(open(hexf,'rb').read()).hexdigest()[:32]}...")

    m = read(os.path.join(MDK, PROJ, f"{PROJ}.map"))
    check("MAP: main.o -> Key_Task_Init", "main.o(.text.main) refers to bsp_key.o(.text.Key_Task_Init)" in m)
    check("MAP: main.o -> J8108_Task_Init", "main.o(.text.main) refers to j8108_task.o(.text.J8108_Task_Init)" in m)
    check("MAP: main.o -> Oled_Task_Init", "refers to oled_task.o(.text.Oled_Task_Init)" in m)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--fast", action="store_true", help="跳过 UV4 构建")
    args = ap.parse_args()

    print(f"verify_dev: {BASE}\n")
    tmp = tempfile.mkdtemp(prefix="hermes-verify-verifydev-")
    try:
        host_tests(tmp)
        static_checks()
        if not args.fast:
            build_and_link()
        else:
            print("---- (--fast：跳过 UV4 构建) ----")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print("\n================ 汇总 ================")
    ok_all = True
    for n, ok in RESULTS:
        print(("PASS  " if ok else "FAIL  ") + n)
        ok_all = ok_all and ok
    print(f"\n共 {len(RESULTS)} 项，失败 {sum(1 for _, o in RESULTS if not o)} 项 ->",
          "ALL PASS" if ok_all else "FAILED")
    return 0 if ok_all else 1


if __name__ == "__main__":
    sys.exit(main())
