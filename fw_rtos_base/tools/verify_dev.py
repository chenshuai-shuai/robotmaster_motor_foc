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
  CK(fabsf(d16(p,-12.56f,12.56f))<0.002f, "decode p ~ 0 (dual-encoder +-12.56rad)"); CK(fabsf(d12(v,-45.0f,45.0f)-10.0f)<0.05f, "decode v ~ 10");
  memcpy(dev->fb.raw,fb,8); J8108_Update();
  CK(fabsf(dev->fb.pos+0.0345f)<0.005f, "fb POS ~ -0.0345rad (dual-encoder doc: +-12.56rad)");
  CK(dev->fb.err==0x01u, "fb byte0 = ERR bits (dual-encoder doc)");
  CK(strcmp(J8108_ErrStr(0x00u), "OK")==0 && strcmp(J8108_ErrStr(0x40u), "T-COIL")==0 &&
     strcmp(J8108_ErrStr(0x80u), "OVERLOAD")==0, "ERR bit -> label");
  CK(fabsf(dev->fb.torque-0.198f)<0.01f, "fb T ~ 0.198Nm (doc)");
  CK(fabsf(dev->fb.t_mos-29.41f)<0.2f, "fb TMos ~ 29.41C (doc)");
  CK(fabsf(dev->fb.t_rotor-26.67f)<0.2f, "fb TMotor ~ 26.67C (doc)");
  J8108_SendMIT(0.0f,100.0f,0.0f,0.0f,0.0f); v=(uint16_t)((cap[2]<<4)|(cap[3]>>4));
  CK(v==0x0FFF, "v=100 clamped to 0xFFF");
  J8108_SendMIT(0.1f,0.0f,0.0f,0.0f,0.0f); p=(uint16_t)((cap[0]<<8)|cap[1]);
  CK(fabsf(d16(p,-12.56f,12.56f)-0.1f)<0.01f, "MIT encode round-trip at +0.1rad (dual-encoder range)");
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
    CK(fabsf(s1.pos_deg-(dev->fb.pos*57.29578f))<0.01f, "snapshot: deg = pos x RAD2DEG (output side, no /8)");
    CK(fabsf(s1.vel_dps-(dev->fb.vel*57.29578f))<0.01f, "snapshot: dps = vel x RAD2DEG");
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

# ---- 纯逻辑层宿主机测试：命令行解析 / 控制核 / 按键 UI 策略 ----
CMD_TEST = r"""
#include "stdio.h"
#include "string.h"
#include "cmd_parse.h"
static int F;
#define CK(c, m) do { if (c) printf("  PASS  %s\n", m); else { printf("  FAIL  %s\n", m); F++; } } while (0)
static Cmd_ParseRes_e P(const char *s, Cmd_t *c) { return Cmd_ParseLine(s, (uint16_t)strlen(s), c); }
int main(void)
{
    Cmd_t c;
    float v;

    CK(P("#PING", &c) == CMD_PARSE_OK && c.id == CMD_PING, "#PING recognized");
    CK(P("#ping", &c) == CMD_PARSE_OK && c.id == CMD_PING, "command name case-insensitive");
    CK(P("#V 90", &c) == CMD_PARSE_OK && c.nf == 1u && c.f[0] > 89.9f && c.f[0] < 90.1f, "#V 90 -> 1 numeric arg");
    CK(P("#P -168.75 20 1.0", &c) == CMD_PARSE_OK && c.nf == 3u && c.f[0] < -168.7f && c.f[0] > -168.8f, "#P 3 args (negative decimal)");
    CK(P("#MODE POS", &c) == CMD_PARSE_OK && c.nw == 1u && cmd_word_is(&c, 0u, "pos") == 1u, "#MODE POS -> keyword (ci compare)");
    CK(P("#SET kd_damp 1.25", &c) == CMD_PARSE_OK && c.nw == 1u && cmd_word_is(&c, 0u, "KD_DAMP") == 1u && c.f[0] > 1.24f, "#SET key+value");
    CK(P("#LIM P -170 170", &c) == CMD_PARSE_OK && c.nw == 1u && c.nf == 2u, "#LIM P min max");
    CK(P("#SETP 0.1 -2.5 20 1 0.5", &c) == CMD_PARSE_OK && c.nf == 5u, "#SETP needs 5 numeric args");
    CK(P("[j8108] I: a log line", &c) == CMD_PARSE_NOTCMD, "log line -> NOTCMD (ignored)");
    CK(P("@TEL ms=1", &c) == CMD_PARSE_NOTCMD, "@reply line -> NOTCMD");
    CK(P("#NOPE", &c) == CMD_PARSE_UNKNOWN, "unknown -> UNKNOWN (@ERR 1)");
    CK(P("#V 1.2.3", &c) == CMD_PARSE_BADARG, "malformed number -> BADARG (@ERR 2)");
    CK(P("#V 1e3", &c) == CMD_PARSE_BADARG, "exponent unsupported -> BADARG");
    CK(P("#V", &c) == CMD_PARSE_OK && c.nf == 0u, "#V no args parses (dispatcher rejects)");
    CK(P("#V 1 2 3 4 5 6 7", &c) == CMD_PARSE_BADARG, "too many numeric args -> BADARG");
    CK(P("#V -0.5", &c) == CMD_PARSE_OK && c.f[0] < 0.0f, "negative arg");
    CK(cmd_atof("0", 1u, &v) == 1u && v == 0.0f, "atof zero");
    CK(cmd_atof("-.5", 3u, &v) == 1u && v < -0.49f && v > -0.51f, "atof -.5");
    CK(cmd_atof("100.", 4u, &v) == 1u && v > 99.9f, "atof trailing dot");
    CK(cmd_atof("", 0u, &v) == 0u, "atof empty -> fail");
    printf("  ==> %s (fail=%d)\n", F ? "FAIL" : "ALL PASS", F);
    return F ? 1 : 0;
}
"""

CTRL_TEST = r"""
#include "stdio.h"
#include "string.h"
#include "ctrl_core.h"
static int F;
#define CK(c, m) do { if (c) printf("  PASS  %s\n", m); else { printf("  FAIL  %s\n", m); F++; } } while (0)
static float fa(float v) { return (v < 0.0f) ? -v : v; }
static const float D2R = 0.017453292f;
int main(void)
{
    Ctrl_Params_t p;
    Ctrl_State_t s;
    Ctrl_Frame_t f;
    int i;
    uint16_t e;

    Ctrl_ParamsDefault(&p);
    Ctrl_Init(&s, &p);

    CK(p.wd_ms == 200u && p.kd_damp == 1.0f && p.kp_pos == 20.0f && p.tmax_nm == 7.5f && p.setp_unlocked == 0u &&
       p.kp_v <= 0.01f && p.trate_nm_s > 0.0f,
       "defaults: wd=200 kd_damp=1 kp_pos=20 tmax=7.5 setp locked + kp_v<=0.01 + trate>0 (bench-safe)");

    Ctrl_Step(&s, &p, 0.0f, 0.0f, 5u, &f);
    CK(f.send == 0u, "disabled+IDLE -> no frame");
    s.enabled = 1u;
    Ctrl_Step(&s, &p, 0.0f, 0.0f, 5u, &f);
    CK(f.send == 0u, "enabled but IDLE -> still no frame (no TX when idle)");

    Ctrl_SetMode(&s, CTRL_MODE_DAMP, 10.0f, 0.0f);
    Ctrl_Step(&s, &p, 10.0f, 0.0f, 5u, &f);
    CK(f.send == 1u && f.kp == 0.0f && f.kd == p.kd_damp && f.t_nm == 0.0f, "DAMP: kp=0 kd=1 t=0 (pure damping)");

    Ctrl_SetMode(&s, CTRL_MODE_POS, 12.5f, 0.0f);
    CK(fa(s.set_applied - 12.5f) < 0.01f, "POS enter: setpoint preset to current angle (no jump)");
    Ctrl_SetPos(&s, 90.0f);
    Ctrl_Step(&s, &p, 12.5f, 0.0f, 10u, &f);
    CK(f.kp == p.kp_pos && f.kd == p.kd_pos, "POS: Kp AND Kd both sent (doc: Kp w/o Kd oscillates)");
    CK(fa(f.p_rad - 90.0f * D2R) > 0.1f, "POS: slew-limited (not instant jump)");
    for (i = 0; i < 200; i++) { Ctrl_Step(&s, &p, 12.5f, 0.0f, 10u, &f); }
    CK(fa(f.p_rad - 90.0f * D2R) < 0.01f, "POS: reaches target after slew");
    Ctrl_SetPos(&s, 9999.0f);
    for (i = 0; i < 600; i++) { Ctrl_Step(&s, &p, 12.5f, 0.0f, 10u, &f); }
    CK(fa(f.p_rad - p.pmax_deg * D2R) < 0.01f, "POS: clamped to soft limit pmax");
    CK((s.ev & CTRL_EV_LIMIT) != 0u, "LIMIT event raised when setpoint is clamped");
    Ctrl_SetMode(&s, CTRL_MODE_POS, 100.0f, 0.0f);
    Ctrl_SetPos(&s, 100.5f);
    Ctrl_Step(&s, &p, 100.0f, 0.0f, 10u, &f);
    CK(s.inpos == 1u, "POS: inpos asserted inside band");

    Ctrl_SetMode(&s, CTRL_MODE_TORQUE, 0.0f, 0.0f);
    Ctrl_SetTorque(&s, 99.0f);
    Ctrl_Step(&s, &p, 0.0f, 0.0f, 5u, &f);
    CK(fa(f.t_nm) < 1.0f, "TORQUE: first step is rate-limited (no instant 7.5Nm kick)");
    for (i = 0; i < 200; i++) { Ctrl_Step(&s, &p, 0.0f, 0.0f, 5u, &f); }
    CK(fa(f.t_nm - p.tmax_nm) < 0.001f && f.kp == 0.0f && f.kd == 0.0f, "TORQUE: reaches and holds tmax after slew, kp=kd=0");

    /* ★ 力矩斜率限制（实机 2026-09-18 抖动修复的核心） */
    Ctrl_Init(&s, &p); s.enabled = 1u; Ctrl_SetMode(&s, CTRL_MODE_TORQUE, 0.0f, 0.0f);
    Ctrl_SetTorque(&s, 7.5f);
    Ctrl_Step(&s, &p, 0.0f, 0.0f, 5u, &f);
    CK(fa(f.t_nm - (p.trate_nm_s * 0.005f)) < 0.001f, "TRATE: torque step limited to trate*dt (=0.1Nm @200Hz)");

    /* ★ 事件边沿锁存：持续夹紧只报一次（防 @EVT 洪泛占满串口） */
    {
        int hits = 0;
        Ctrl_Init(&s, &p); s.enabled = 1u; Ctrl_SetMode(&s, CTRL_MODE_TORQUE, 0.0f, 0.0f);
        Ctrl_SetTorque(&s, 7.5f);
        for (i = 0; i < 50; i++) { Ctrl_Step(&s, &p, 0.0f, 0.0f, 5u, &f); if ((Ctrl_TakeEvents(&s) & CTRL_EV_LIMIT) != 0u) { hits++; } }
        CK(hits == 1, "LIMIT latched: 50 clamped cycles -> exactly 1 event (was: 50 events/250ms)");
    {
        /* ★ 关键回归（实机 2026-09-18）：斜率限制"时紧时松"会反复解锁纯边沿锁存 → 刷屏。
         *   抖动 50 次（夹紧/不夹紧交替）必须只出 1 条事件。 */
        int hits = 0;
        Ctrl_Init(&s, &p); s.enabled = 1u; Ctrl_SetMode(&s, CTRL_MODE_TORQUE, 0.0f, 0.0f);
        Ctrl_SetTorque(&s, 7.5f);
        int pre = 0;
        Ctrl_SetTorque(&s, 1.0f);
        for (i = 0; i < 60; i++) { Ctrl_Step(&s, &p, 0.0f, 0.0f, 5u, &f); (void)Ctrl_TakeEvents(&s); } /* 先收敛并清事件 */
        for (i = 0; i < 100; i++) {
            /* 真实抖动模型：目标交替"斜率内(不夹紧)"与"超斜率(夹紧)" → 每 5ms 翻转一次 */
            Ctrl_SetTorque(&s, ((i % 2) == 0) ? 1.05f : 1.60f);
            Ctrl_Step(&s, &p, 0.0f, 0.0f, 5u, &f);
            if ((Ctrl_TakeEvents(&s) & CTRL_EV_LIMIT) != 0u) { hits++; }
        }
        (void)pre;
        CK(hits == 0, "LIMIT debounce: clamp flutter 100x -> no repeat report (first episode already reported)");
    }
    {
        /* 消退去抖：夹紧 → 连续 1s 不夹紧 → 再夹紧 = 允许新事件（真正的两次事件） */
        int hits = 0;
        Ctrl_Init(&s, &p); s.enabled = 1u; Ctrl_SetMode(&s, CTRL_MODE_TORQUE, 0.0f, 0.0f);
        Ctrl_SetTorque(&s, 7.5f);
        for (i = 0; i < 10; i++) { Ctrl_Step(&s, &p, 0.0f, 0.0f, 5u, &f); if ((Ctrl_TakeEvents(&s) & CTRL_EV_LIMIT) != 0u) { hits++; } }
        Ctrl_SetTorque(&s, 0.0f);
        for (i = 0; i < 250; i++) { Ctrl_Step(&s, &p, 0.0f, 0.0f, 5u, &f); (void)Ctrl_TakeEvents(&s); } /* 1.25s 不夹紧 */
        Ctrl_SetTorque(&s, 7.5f);
        for (i = 0; i < 10; i++) { Ctrl_Step(&s, &p, 0.0f, 0.0f, 5u, &f); if ((Ctrl_TakeEvents(&s) & CTRL_EV_LIMIT) != 0u) { hits++; } }
        CK(hits == 2, "LIMIT debounce: 1s clear -> re-arms, second episode reports again");
    /* ★ 摩擦前馈（库仑摩擦补偿）：必须按**设定点方向**取号，否则反向时变成"反向拖拽" */
    Ctrl_Init(&s, &p);
    p.tff_nm = 0.15f;
    s.enabled = 1u;
    Ctrl_SetMode(&s, CTRL_MODE_SPEED, 0.0f, 0.0f);
    Ctrl_SetVel(&s, 10.0f);
    for (i = 0; i < 30; i++) { Ctrl_Step(&s, &p, 0.0f, 0.0f, 5u, &f); }
    CK(f.t_nm > 0.10f, "TFF: +setpoint -> positive friction feedforward (0.15Nm + P term)");
    Ctrl_SetVel(&s, -10.0f);
    for (i = 0; i < 60; i++) { Ctrl_Step(&s, &p, 0.0f, 0.0f, 5u, &f); }
    CK(f.t_nm < -0.10f, "TFF: -setpoint -> negative friction feedforward (sign follows direction)");
    p.tff_nm = 0.0f;
    }
    }
    {
        int hits = 0;
        Ctrl_Init(&s, &p); s.enabled = 1u; Ctrl_SetMode(&s, CTRL_MODE_SPEED, 0.0f, 0.0f);
        for (i = 0; i < 20; i++) { (void)Ctrl_Protect(&s, &p, 0.0f, 0.0f, 0.0f, 30.0f, 30.0f, 0x40u, 5u); if ((Ctrl_TakeEvents(&s) & CTRL_EV_MOTERR) != 0u) { hits++; } }
        CK(hits == 1, "MOTERR latched: 20 cycles of error bits -> exactly 1 event");
    }
    CK(Ctrl_ParamsDefault != NULL, "params default api present");

    Ctrl_SetMode(&s, CTRL_MODE_SPEED, 0.0f, 0.0f);
    Ctrl_SetVel(&s, 60.0f);
    Ctrl_Step(&s, &p, 0.0f, 0.0f, 5u, &f);
    CK(f.kp == 0.0f && f.kd == p.kd_damp, "SPEED: kp=0 + damping kd");
    CK(f.t_nm > 0.0f, "SPEED: PI gives positive torque for positive error");
    for (i = 0; i < 200; i++) { Ctrl_Step(&s, &p, 0.0f, 0.0f, 5u, &f); }
    CK(f.t_nm <= p.tmax_nm + 0.001f && f.t_nm >= -p.tmax_nm, "SPEED: torque clamped (anti-windup)");

    Ctrl_Ping(&s, 1000u);
    CK(Ctrl_Heartbeat(&s, &p, 1100u) == 0u, "heartbeat ok inside wd");
    CK(Ctrl_Heartbeat(&s, &p, 1300u) == 1u && s.mode == CTRL_MODE_DAMP, "heartbeat timeout -> DAMP");
    CK(s.enabled == 1u, "timeout does NOT disable (no brake -> stay damped)");

    Ctrl_Init(&s, &p); s.enabled = 1u; Ctrl_SetMode(&s, CTRL_MODE_POS, 0.0f, 0.0f); s.set_applied = 0.0f;
    e = 0u;
    for (i = 0; i < 60; i++) { e |= Ctrl_Protect(&s, &p, 100.0f, 0.0f, 0.0f, 30.0f, 30.0f, 0u, 10u); }
    CK((e & CTRL_EV_FOLLOW) != 0u && s.mode == CTRL_MODE_DAMP, "FOLLOW: 30deg error for 500ms -> DAMP");

    Ctrl_Init(&s, &p); s.enabled = 1u; Ctrl_SetMode(&s, CTRL_MODE_SPEED, 0.0f, 0.0f);
    e = 0u;
    for (i = 0; i < 110; i++) { e |= Ctrl_Protect(&s, &p, 0.0f, 2.0f, 9.0f, 30.0f, 30.0f, 0u, 10u); }
    CK((e & CTRL_EV_STALL) != 0u, "STALL: high torque + low speed for 1s -> event");

    Ctrl_Init(&s, &p); s.enabled = 1u; Ctrl_SetMode(&s, CTRL_MODE_SPEED, 0.0f, 0.0f);
    e = Ctrl_Protect(&s, &p, 0.0f, 0.0f, 0.0f, 90.0f, 30.0f, 0u, 10u);
    CK((e & CTRL_EV_TEMP) != 0u && s.mode == CTRL_MODE_DAMP, "TEMP: above stop threshold -> DAMP");

    Ctrl_Init(&s, &p); s.enabled = 1u; Ctrl_SetMode(&s, CTRL_MODE_SPEED, 0.0f, 0.0f);
    e = Ctrl_Protect(&s, &p, 0.0f, 0.0f, 0.0f, 30.0f, 30.0f, 0x40u, 10u);
    CK((e & CTRL_EV_MOTERR) != 0u, "MOTOR ERR bit (byte0) -> event");

    Ctrl_Estop(&s);
    CK(s.enabled == 0u && s.estop == 1u, "ESTOP: disabled + estop latched");
    CK(strcmp(Ctrl_ModeStr(CTRL_MODE_POS), "POS") == 0 && strcmp(Ctrl_ModeStr(CTRL_MODE_DAMP), "DAMP") == 0 &&
       strcmp(Ctrl_ModeStr(CTRL_MODE_IDLE), "IDLE") == 0, "mode strings");

    printf("  ==> %s (fail=%d)\n", F ? "FAIL" : "ALL PASS", F);
    return F ? 1 : 0;
}
"""

UIACT_TEST = r"""
#include "stdio.h"
#include "string.h"
#include "ui_action.h"
static int F;
#define CK(c, m) do { if (c) printf("  PASS  %s\n", m); else { printf("  FAIL  %s\n", m); F++; } } while (0)
int main(void)
{
    CK(Ui_ActionDecide(UI_GES_CLICK, 0u, 2000u) == UI_ACT_PAGE_NEXT, "click -> next page");
    CK(Ui_ActionDecide(UI_GES_DOUBLE, 0u, 2000u) == UI_ACT_PAGE_PREV, "double -> prev page");
    CK(Ui_ActionDecide(UI_GES_HOLD, 1999u, 2000u) == UI_ACT_NONE, "hold 1999ms -> nothing");
    CK(Ui_ActionDecide(UI_GES_HOLD, 2000u, 2000u) == UI_ACT_PAGE_HOME, "hold >=2s -> home");
    CK(Ui_ActionDecide(UI_GES_STUCK, 9000u, 2000u) == UI_ACT_NONE, "stuck -> voided (no action)");
    CK(Ui_PageNext(0u, 4u) == 1u && Ui_PageNext(3u, 4u) == 0u, "page next wraps");
    CK(Ui_PagePrev(0u, 4u) == 3u && Ui_PagePrev(2u, 4u) == 1u, "page prev wraps");
    CK(Ui_PageNext(0u, 1u) == 0u && Ui_PagePrev(0u, 1u) == 0u, "single page stays 0");
    CK(UI_HOME_HOLD_MS == 2000u, "home hold threshold = 2s");
    CK(strcmp(Ui_ActionStr(UI_ACT_PAGE_NEXT), "PAGE_NEXT") == 0, "action string mapping");
    printf("  ==> %s (fail=%d)\n", F ? "FAIL" : "ALL PASS", F);
    return F ? 1 : 0;
}
"""


DISP_TEST = r"""
#include "stdio.h"
#include "disp_geom.h"
static int F;
#define CK(c, m) do { if (c) printf("  PASS  %s\n", m); else { printf("  FAIL  %s\n", m); F++; } } while (0)
static int16_t PX[64], PY[64]; static int NP;
static void plot(int16_t x, int16_t y, void *ctx) { (void)ctx; if (NP < 64) { PX[NP] = x; PY[NP] = y; NP++; } }
int main(void)
{
    Disp_GeomRect_t r;

    /* ---- 脏矩形：并集 / 首次 ---- */
    Disp_GeomReset(&r);
    CK(r.valid == 0, "dirty: reset -> empty");
    Disp_GeomAdd(&r, 10, 20, 5, 5, 128, 160, 40);
    CK(r.valid == 1 && r.x0 == 10 && r.y0 == 20 && r.x1 == 14 && r.y1 == 24, "dirty: first add = its own box");
    Disp_GeomAdd(&r, 100, 100, 10, 10, 128, 160, 0);
    CK(r.x0 == 10 && r.y0 == 20 && r.x1 == 109 && r.y1 == 109, "dirty: union of two boxes (ratio 0 = no escalation)");
    CK(Disp_GeomArea(&r) == 100u * 90u, "dirty: area = union box");

    /* ---- 裁剪：负坐标/越界/完全在外 ---- */
    Disp_GeomReset(&r);
    Disp_GeomAdd(&r, -10, -10, 20, 20, 128, 160, 40);
    CK(r.x0 == 0 && r.y0 == 0 && r.x1 == 9 && r.y1 == 9, "dirty: negative rect clipped to origin");
    Disp_GeomReset(&r);
    Disp_GeomAdd(&r, 200, 10, 10, 10, 128, 160, 40);
    CK(r.valid == 0, "dirty: fully off-screen rect ignored (no dirty)");
    Disp_GeomReset(&r);
    Disp_GeomAdd(&r, 120, 150, 100, 100, 128, 160, 40);
    CK(r.x0 == 120 && r.y0 == 150 && r.x1 == 127 && r.y1 == 159, "dirty: right/bottom clipped to screen");

    /* ---- 满屏升级（面积 ≥ ratio%）---- */
    Disp_GeomReset(&r);
    Disp_GeomAdd(&r, 0, 0, 128, 80, 128, 160, 40);
    CK(r.x0 == 0 && r.y0 == 0 && r.x1 == 127 && r.y1 == 159, "dirty: >=40%% -> escalate to full screen");
    Disp_GeomReset(&r);
    Disp_GeomAdd(&r, 0, 0, 128, 32, 128, 160, 40);
    CK(r.x1 == 127 && r.y1 == 31, "dirty: <40%% stays partial");
    Disp_GeomReset(&r);
    Disp_GeomAdd(&r, 0, 0, 128, 80, 128, 160, 0);
    CK(r.y1 == 79, "dirty: ratio 0 disables escalation");

    /* ---- 位模取位：纵向 8 点/字节、LSB 在最上面一行 ---- */
    CK(Disp_GeomGlyphBit(0x01, 0) == 1 && Disp_GeomGlyphBit(0x01, 1) == 0, "glyph: 0x01 -> only row0");
    CK(Disp_GeomGlyphBit(0x80, 7) == 1 && Disp_GeomGlyphBit(0x80, 6) == 0, "glyph: 0x80 -> only row7");
    CK(Disp_GeomGlyphBit(0xFF, 8) == 0, "glyph: row>7 -> 0 (no OOB)");

    /* ---- 直线：端点都画 / 点数 / 退化 ---- */
    NP = 0; Disp_GeomLine(0, 5, 9, 5, plot, 0);
    CK(NP == 10 && PX[0] == 0 && PX[9] == 9 && PY[0] == 5, "line: horizontal 0..9 = 10 px, endpoints drawn");
    NP = 0; Disp_GeomLine(3, 3, 3, 3, plot, 0);
    CK(NP == 1, "line: single point = 1 px");
    NP = 0; Disp_GeomLine(0, 0, 3, 3, plot, 0);
    CK(NP == 4 && PX[3] == 3 && PY[3] == 3, "line: 45 deg diagonal = 4 px");
    NP = 0; Disp_GeomLine(0, 0, 4, 2, plot, 0);
    CK(NP == 5, "line: shallow slope count = max(dx,dy)+1");
    NP = 0; Disp_GeomLine(9, 0, 0, 9, plot, 0);
    CK(NP == 10 && PX[0] == 9 && PX[9] == 0, "line: reverse direction still starts at first endpoint");

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
        ("命令行解析 cmd_parse", "cmdparse", ["mcu_bsp/proto/cmd_parse.h"], CMD_TEST),
        ("控制核 ctrl_core", "ctrl", ["mcu_bsp/ctrl/ctrl_core.c", "mcu_bsp/ctrl/ctrl_core.h"], CTRL_TEST),
        ("按键 UI 策略 ui_action", "uiact", ["mcu_bsp/Motor/ui_action.h"], UIACT_TEST),
        # V7：显示层纯逻辑（脏矩形/裁剪/满屏升级/位模取位/Bresenham）— 真源码，宿主机穷举
        ("显示几何 disp_geom", "dispgeom", ["mcu_bsp/disp/disp_geom.c", "mcu_bsp/disp/disp_geom.h"], DISP_TEST),
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
    need = {"FEATURE_J8108", "FEATURE_KEY", "FEATURE_DISP_UI", "FEATURE_MONITOR_TASK",
            "FEATURE_SERIAL_CTRL", "FEATURE_LED_TASK", "FEATURE_SD_CARD", "FEATURE_SD_CLI",
            "FEATURE_CAR_TASKS", "FEATURE_VERBOSE_LOG", "FEATURE_DISP_SH1106_I2C",
            "FEATURE_DISP_ST7735S_SPI"}
    check("feature_config.h: 12 个功能宏齐全（含显示驱动选择）", need <= set(macros),
          str(sorted(need - set(macros))))
    check("feature_config.h: 4 个预设组合 + CFG_PROFILE 可被 -D 覆盖（组合编译矩阵的前提）",
          all(f"#define {p} " in fc for p in ("PROFILE_FULL", "PROFILE_DISP_DEV",
                                              "PROFILE_MOTOR_DEV", "PROFILE_MINIMAL"))
          and "#ifndef CFG_PROFILE" in fc)
    check("feature_config.h: 依赖矩阵齐全（MONITOR→DISP_UI / MONITOR→KEY / SD_CLI→SD_CARD / 显示驱动唯一）",
          all(s in fc for s in ("FEATURE_MONITOR_TASK requires FEATURE_DISP_UI",
                                "FEATURE_MONITOR_TASK requires FEATURE_KEY",
                                "FEATURE_SD_CLI requires FEATURE_SD_CARD",
                                "needs exactly ONE display driver",
                                "two display drivers enabled")))
    _mc = read(os.path.join(BASE, "Core", "Src", "main.c"))
    check("安全告警 R7：关闭安全模块的编译期告警在组装根 main.c（且已从共享头移出）",
          "motor control and protection are OFF" in _mc
          and "motor control and protection are OFF" not in fc)
    dv = re.findall(r"#define\s+FEATURE_DISP_SH1106_I2C\s+(\d+)u", fc)
    ds = re.findall(r"#define\s+FEATURE_DISP_ST7735S_SPI\s+(\d+)u", fc)
    check("feature_config.h: 显示驱动恰好选中一个（同一组引脚，不能俩都开）",
          len(dv) == 1 and len(ds) == 1 and ((dv[0] == "1") != (ds[0] == "1")),
          f"sh1106={dv} st7735s={ds}")

    mainc = read(os.path.join(BASE, "Core", "Src", "main.c"))
    live = [ln for ln in mainc.splitlines() if not ln.strip().startswith(("//", "*"))]

    def live_has(needle):
        return any(needle in ln for ln in live)

    check("main.c: Key_Task_Init() 在非注释代码中", live_has("Key_Task_Init();"))
    check("main.c: J8108_Task_Init() 在非注释代码中", live_has("J8108_Task_Init();"))
    check("main.c: 二者均在 #if FEATURE_* 保护块内", "#if FEATURE_KEY" in mainc and "#if FEATURE_J8108" in mainc)
    check("main.c: SD/CLI/小车 已被宏包住", all(f"#if FEATURE_{x}" in mainc for x in ("SD_CARD", "SD_CLI", "CAR_TASKS")))
    # ---- M1 抓到的两个"隐藏依赖"回归（协议发送口 / 串口任务被错误嵌套）----
    check("main.c: Proto_TxInit 在显示宏之前（协议发送口与显示解耦）",
          mainc.index("Proto_TxInit();") < mainc.index("#if FEATURE_DISP_UI"))
    _seg = mainc[mainc.index("#if FEATURE_J8108"):mainc.index("#if FEATURE_SERIAL_CTRL")]
    check("main.c: CmdRx_Task_Init 只由 SERIAL_CTRL 门控（J8108 块已闭合，不再嵌套）",
          "#endif" in _seg and "CmdRx_Task_Init" not in _seg)
    # ---- M2：依赖收口（隐藏依赖不许复活；见 docs/3_过程记录/日志_模块化改造.md M2）----
    _cx = read(os.path.join(BASE, "Task", "Src", "CmdRx_Task.c"))
    _jt = read(os.path.join(BASE, "Task", "Src", "J8108_Task.c"))
    check("M2: #TEL 在无监控任务时回 @ERR 4（不许假 @OK —— 回包必须与行为一致）",
          "feature=monitor off (telemetry has no producer)" in _cx
          and "#if FEATURE_MONITOR_TASK" in _cx)
    check("M2: 电机模块关闭时 #EN 类命令回 @ERR 4（不许让上位机误判成接线/NOACK）",
          "feature=j8108 off (this firmware has no motor module)" in _cx
          and "J8108_RES_DISABLED" in _cx)
    check("M2: J8108_Task.c 有'显式停用桩'（服务模块被通用层调用 → 允许桩，但必须诚实报停用）",
          re.search(r"#else\s+/\* !FEATURE_J8108", _jt) is not None
          and "J8108_RES_DISABLED" in _jt and "#if FEATURE_J8108" in _jt)
    check("M2: J8108_Task.c 不再依赖 UI 策略头（单向依赖：控制层不依赖 UI 层）",
          re.search(r'#include\s+"ui_action\.h"', _jt) is None)
    for _l in [ln for ln in mainc.splitlines() if "@BOOT" in ln and "LOG_I" in ln]:
        _fmt = re.findall(r'"([^"]*)"', _l)[0]
        check(f"横幅单行 ≤110 字符（LOG_FMT_BUF_SIZE=128 会截断，M1 实测）: {_fmt[:34]}…",
              len(_fmt) <= 110, f"{len(_fmt)} chars")
    # ---- M3：文件级隔离（R3）——每个"模块实现文件"都必须被自己的宏包住 ----
    M3_WRAP = {
        "Task/Src/Oled_Task.c": "FEATURE_DISP_UI",
        "Task/Src/Monitor_Task.c": "FEATURE_MONITOR_TASK",
        "Task/Src/CmdRx_Task.c": "FEATURE_SERIAL_CTRL",
        "Task/Src/Led_Task.c": "FEATURE_LED_TASK",
        "Task/Src/SdCard_Task.c": "FEATURE_SD_CARD",
        "Task/Src/J8108_Task.c": "FEATURE_J8108",
        "mcu_bsp/key/bsp_key.c": "FEATURE_KEY",
        "mcu_bsp/oled/OLED.c": "FEATURE_DISP_SH1106_I2C",
        # 内置 ASCII 点阵被**两个驱动共用** → 它自己的开关是派生宏 FEATURE_DISP_FONTS
        # （否则选中彩屏时它被编成空对象，驱动 B 链接期找不到字模）
        "mcu_bsp/oled/OLED_Data.c": "FEATURE_DISP_FONTS",
        # 显示抽象层（S1/S2）：两个驱动各被自己的宏包住；disp_geom.c 是共用纯逻辑，不包宏
        "mcu_bsp/disp/disp_sh1106_i2c.c": "FEATURE_DISP_SH1106_I2C",
        "mcu_bsp/disp/disp_st7735s_spi.c": "FEATURE_DISP_ST7735S_SPI",
        "mcu_bsp/sd/sd_sdio.c": "FEATURE_SD_CARD",
        "mcu_bsp/fs/sd_fs.c": "FEATURE_SD_CARD",
        "mcu_bsp/fs/sd_diskio.c": "FEATURE_SD_CARD",
        "mcu_bsp/cli/sd_cli.c": "FEATURE_SD_CLI",
        "Middlewares/Third_Party/FatFs/src/ff.c": "FEATURE_SD_CARD",
        "Middlewares/Third_Party/FatFs/src/option/ccsbcs.c": "FEATURE_SD_CARD",
    }
    _bad_wrap = []
    for _rel, _mac in M3_WRAP.items():
        _txt = read(os.path.join(BASE, _rel.replace("/", os.sep)))
        _head = "\n".join(_txt.splitlines()[:60])
        if (re.search(rf"#\s*if.*\b{_mac}\b", _head) is None) or (f"#endif /* {_mac} */" not in _txt):
            _bad_wrap.append(_rel)
    check(f"M3: {len(M3_WRAP)} 个模块实现文件均被自己的宏包住（首 60 行内 #if、末尾 #endif）",
          not _bad_wrap, ",".join(_bad_wrap))
    # ★ 顺序陷阱（2026-09-19 实踩）：宏必须在 #include 之后才判断。反过来写（#if 在前）时该宏
    #   还没定义 → 预处理器按 0 处理 → 整个文件被**静默**编成空对象，编译期无错，
    #   链接期才炸 `L6218E: Undefined symbol ...`。这条断言把此类问题钉在静态检查里。
    _bad_order = []
    for _rel, _mac in M3_WRAP.items():
        _ls = read(os.path.join(BASE, _rel.replace("/", os.sep))).splitlines()[:60]
        _i_if = next((k for k, l in enumerate(_ls) if re.search(rf"^\s*#\s*if.*\b{_mac}\b", l)), -1)
        _i_inc = next((k for k, l in enumerate(_ls) if re.match(r"^\s*#\s*include", l)), -1)
        if (_i_if < 0) or (_i_inc < 0) or (_i_inc > _i_if):
            _bad_order.append(f"{_rel}(#if@{_i_if} include@{_i_inc})")
    check("M3: 每个文件都是\"先 include 再 #if\"（反例=整文件静默空对象，链接期才报 L6218E）",
          not _bad_order, ",".join(_bad_order))
    _fc_cfg = read(os.path.join(BASE, "Task", "Inc", "feature_config.h"))
    check("M3: 编译期告警只在组装根 main.c 报一次（共享头里放 #warning 会变成 N 条同文噪声）",
          "#warning" not in _fc_cfg and "#warning" in mainc and "!FEATURE_J8108" in mainc)
    # ---- M4：模块自检 #ST ----
    _cp4 = read(os.path.join(BASE, "mcu_bsp", "proto", "cmd_parse.h"))
    _man4 = read(os.path.join(BASE, "docs", "1_规则（既定事实）/操作手册_串口命令.md"))
    _cmd4 = read(os.path.join(BASE, "Task", "Src", "CmdRx_Task.c"))
    check("M4: #ST 进了解析表且手册有对应行", '"ST"' in _cp4 and "#ST" in _man4)
    M4_ST = {
        # 显示自检已上移到屏抽象层：契约声明 + 两个驱动各一份实现（页面层不再有自己的自检）
        "mcu_bsp/disp/disp_port.h": "Disp_SelfTest",
        "mcu_bsp/disp/disp_sh1106_i2c.c": "Disp_SelfTest",
        "mcu_bsp/disp/disp_st7735s_spi.c": "Disp_SelfTest",
        "Task/Src/Led_Task.c": "Led_SelfTest",
        "mcu_bsp/key/bsp_key.c": "Key_SelfTest",
        "Task/Src/Monitor_Task.c": "Monitor_SelfTest",
        "Task/Src/J8108_Task.c": "J8108_SelfTest",
    }
    _miss = [f for f, fn in M4_ST.items() if fn not in read(os.path.join(BASE, f.replace("/", os.sep)))]
    check(f"M4: {len(M4_ST)} 个模块都实现了 Xxx_SelfTest()（非破坏性只读）", not _miss, ",".join(_miss))
    check("M4: #ST disp 挂的是屏抽象层 Disp_SelfTest()（驱动不许返回 0，0 保留给'未编译'）",
          "return Disp_SelfTest();" in _cmd4 and "Disp_SelfTest(void)" in
          read(os.path.join(BASE, "mcu_bsp", "disp", "disp_port.h")))
    check("M4: 自检码语义 0/1/2/3 + 未编译走 @ERR 9 code=255（实现与手册一致）",
          ("code=255" in _cmd4) and ("self-test FAIL" in _cmd4)
          and ("`0`=本固件未编译" in _man4) and ("`1`=OK" in _man4))
    _nst = len(re.findall(r'\{\s*"(?:disp|led|key|can|j8108|monitor)",\s*st_', _cmd4))
    check("M4: #ST 汇总恰好 6 个模块占位（≤6/行，防 127B 截断老坑）", _nst == 6, f"{_nst} 个")
    # 台架实测抓到过：`#ST` 汇总回包写成 `@OK disp=1 ...`（漏了命令名，与手册 `@OK ST disp=1 ...` 不一致）。
    # 这类"回包与协议口径不一致"必须机械防住：所有 reply_ok 的首字段要么是大写命令名，要么是变量里的命令名。
    _bad_tag = []
    for _m in re.finditer(r'reply_ok\(\s*"([^"]*)"', _cmd4):
        _fmt = _m.group(1)
        _first = _fmt.split(" ")[0] if _fmt else ""
        if not (re.fullmatch(r"[A-Z][A-Z0-9_]*", _first) or _first == "%s"):
            _bad_tag.append(_fmt[:44])
    check("M4: 所有 @OK 回包首字段都是命令名（防'漏命令名'的协议口径不一致）",
          not _bad_tag, "; ".join(_bad_tag))
    # ---- M5：板级常量抽离（K5）——模块里不许再出现裸引脚值 ----
    _bc = os.path.join(BASE, "Task", "Inc", "board_config.h")
    _bc_txt = read(_bc)
    check("M5: board_config.h 存在且含 LED/按键/屏幕口三组常量 + 口位注释",
          all(k in _bc_txt for k in ("BRD_LED_GRN_PIN", "BRD_KEY_PIN", "BRD_SCR_CS_PIN",
                                     "BRD_SCR_I2C_SDA_PIN", "BRD_LED_EXT_FIRST_PIN",
                                     "pin1", "pin7", "换板子/换屏只改本文件")))
    # 允许例外（已登记在 board_config.h 头部）：不在编译集内的 dc_motor.c、SD 关闭态的 sd_sdio.h
    _M5_OK = {"mcu_bsp\\Motor\\dc_motor.c", "mcu_bsp\\sd\\sd_sdio.h",
              "Task\\Inc\\board_config.h"}
    _pin_re = re.compile(r"GPIO[A-K]\b|GPIO_PIN_\d+")
    _bare = []
    for _root in ("Task", "mcu_bsp"):
        for _dp, _, _fns in os.walk(os.path.join(BASE, _root)):
            for _fn in _fns:
                if not _fn.endswith((".c", ".h")):
                    continue
                _p = os.path.join(_dp, _fn)
                _rel = os.path.relpath(_p, BASE)
                if _rel in _M5_OK:
                    continue
                _txt = open(_p, encoding="utf-8", errors="replace").read()
                _txt = re.sub(r"/\*.*?\*/", "", _txt, flags=re.S)   # 去块注释
                _txt = re.sub(r"//[^\n]*", "", _txt)                # 去行注释
                if _pin_re.search(_txt):
                    _bare.append(_rel)
    check("M5: Task/ 与 mcu_bsp/ 下的裸引脚值为 0（只允许在 board_config.h，例外已登记）",
          not _bare, ",".join(_bare))
    # ---- V1/V2/V4：显示抽象层（S1/S2；逐条对应编码方案 §9）----
    # V1：两个驱动宏必须可被 -D 覆盖（否则组合矩阵永远编不到"没被选中那个驱动"的函数体）
    check("V1: 显示驱动宏在 #ifndef 里（-D 可覆盖，组合矩阵才扫得到彩屏驱动）",
          (len(re.findall(r"#ifndef\s+FEATURE_DISP_SH1106_I2C", _fc_cfg)) == 1)
          and (len(re.findall(r"#ifndef\s+FEATURE_DISP_ST7735S_SPI", _fc_cfg)) == 1))
    # V2：UI 层白名单 —— "换屏不改页面"必须是机制，不是自觉（剥注释后再判定）
    _ui = read(os.path.join(BASE, "Task", "Src", "Oled_Task.c"))
    _ui_code = re.sub(r"/\*.*?\*/", "", _ui, flags=re.S)
    _ui_code = re.sub(r"//[^\n]*", "", _ui_code)
    _bad_ui = [t for t in ("OLED_", "HAL_GPIO", "SPI1", "hspi", "OLED.h", "disp_st7735s")
               if t in _ui_code]
    check("V2: UI 层(Oled_Task.c)只碰 Disp_*（不出现驱动私有符号，剥注释后判定）",
          not _bad_ui, ",".join(_bad_ui))
    check("V2: 页面层版面由 Disp_Info() 算出（不写死 128x64/21 列）",
          ("Disp_Info()" in _ui) and ("font_w[DISP_FONT_SMALL]" in _ui) and ("s_cols" in _ui))
    # V4：CS 反相双从机（低=LCD / 高=字库）——进字库事务必须把 CS 拉回 LCD，否则之后花屏
    _dh = read(os.path.join(BASE, "mcu_bsp", "disp", "disp_st7735s_spi.h"))
    _dc = read(os.path.join(BASE, "mcu_bsp", "disp", "disp_st7735s_spi.c"))
    check("V4: CS 成对宏存在（低=LCD / 高=字库）",
          ("DISP_CS_SEL_LCD()" in _dh) and ("DISP_CS_SEL_FONT()" in _dh))
    # 断言按**函数体**取（不能用"全文件里 FONT 之后 700 字符内有 LCD"：字节发送路径里也有 FONT
    # ——例程式 CS 风格会先 LCD 后 FONT，正反两种顺序都存在，全局窗口法必然误报）。
    _nl = chr(10)
    _if = _dc.find("static void font_probe")
    _jf = _dc.find(_nl + "}", _if) if _if >= 0 else -1
    _fp = _dc[_if:_jf + 2] if (_if >= 0 and _jf > 0) else ""
    _fp_font = _fp.find("DISP_CS_SEL_FONT()")
    _fp_lcd = _fp.find("DISP_CS_SEL_LCD()", _fp_font if _fp_font >= 0 else 0)
    check("V4: font_probe 里进字库事务后把 CS 拉回 LCD（事务成对，防花屏）",
          (_fp_font >= 0) and (_fp_lcd > _fp_font), f"font@{_fp_font} lcd@{_fp_lcd}")
    # ★ 状态线铁律（2026-09-19 实机踩过，代价是半小时的接线排查）：
    #   lcd_set_window() 最后一步是 lcd_cmd(0x2C) → RS 留在**命令**态；发像素前必须显式切回**数据**态。
    #   漏了 = 整帧像素被面板当命令吃掉 → 屏整片白、一个像素都写不进，而 pushes=/flush=/spifail= 全绿
    #   （SPI 确实发了、命令确实到了、字库也照样能读——字库没有 RS 脚，天然查不出这个问题）。
    _nl = chr(10)
    _i_pb = _dc.find("static void push_band")
    _j_pb = _dc.find(_nl + "}", _i_pb) if _i_pb >= 0 else -1
    _pb_body = _dc[_i_pb:_j_pb + 2] if (_i_pb >= 0 and _j_pb > 0) else ""
    check("V4: push_band 发像素前重新置 RS=数据态（漏了 = 白屏但所有指标全绿）",
          "DISP_RS_DATA()" in _pb_body)
    # ---- 启动架构铁律（2026-09-19 重构）：长延时/自检必须在任务里，不许卡住调度器启动 ----
    #   实机教训：屏的上电等待/自检写在 main() 的 Disp_Init() 里 → 调度器启动被卡 0.4~4s，
    #   期间日志任务（冲刷开机积压日志）与流水灯任务都还没被调度 → "开机几秒全静默然后一起开始"。
    _mc = read(os.path.join(BASE, "Core", "Src", "main.c"))
    _dt_path = os.path.join(BASE, "Task", "Src", "Disp_Task.c")
    _dt = read(_dt_path) if os.path.exists(_dt_path) else ""
    check("启动架构: main() 里 Disp_Init() 之后紧邻创建显示任务 Disp_Task_Init()",
          ("Disp_Init();" in _mc) and ("Disp_Task_Init();" in _mc) and
          (0 <= _mc.index("Disp_Task_Init();") - _mc.index("Disp_Init();") < 900))
    check("启动架构: 显示自检任务存在且调用契约入口 Disp_BringUp()",
          ("Disp_BringUp()" in _dt) and ("xTaskCreate" in _dt) and ("pdPASS" in _dt))
    _di = _dc.find("void Disp_Init(void)")
    _dj = _dc.find(_nl + "}", _di) if _di >= 0 else -1
    _di_body = _dc[_di:_dj + 2] if (_di >= 0 and _dj > 0) else ""
    check("启动架构: 驱动 Disp_Init() 体内零延时（>10ms 的等待全部搬进 Disp_BringUp）",
          (_di_body != "") and ("disp_delay_us" not in _di_body) and ("HAL_Delay" not in _di_body))
    _bi = _dc.find("void Disp_BringUp(void)")
    _bj = _dc.find(_nl + "}", _bi) if _bi >= 0 else -1
    _bi_body = _dc[_bi:_bj + 2] if (_bi >= 0 and _bj > 0) else ""
    check("启动架构: Disp_BringUp() 承担上电等待 + 整段初始化（含 120ms 上电稳定）",
          ("disp_delay_us(120000u)" in _bi_body) and ("panel_init_seq()" in _bi_body))
    _dtx = _dc.find("void Disp_Text(")
    _djx = _dc.find(_nl + "}", _dtx) if _dtx >= 0 else -1
    check("启动架构: 绘制门控用 s_ready（屏就绪前 UI 绘制空操作，无需同步原语）",
          ("s_ready" in _dc[_dtx:_djx + 2]) if (_dtx >= 0 and _djx > 0) else False)
    # 就绪前必须"报 0 尺寸"：否则页面层的"无变化缓存"会被脏填，等屏就绪后永远不再重绘
    # （2026-09-19 实测：屏停在彩色体检最后一帧纯黑、pushes 不再增长）
    check("启动架构: Disp_Init() 报 0 尺寸（屏不可用期间页面层整体空操作，不污染其重绘缓存）",
          ("s_info.w = 0u" in _di_body) and ("s_info.h = 0u" in _di_body))
    check("启动架构: Disp_BringUp() 在置 ready 前把真实尺寸报回（就绪后页面层全刷一次）",
          ("s_info.w = DISP_ST7735S_W" in _bi_body) and ("s_ready = 1u" in _bi_body))
    # V5 的另一半（对象/MAP）在 build_and_link()；V3 见上（M3_WRAP）
    # ---- V9：FEATURE_* 不许是"僵尸宏"（声明了没人用）----
    _feat = re.findall(r"#define\s+(FEATURE_[A-Z0-9_]+)\s", _fc_cfg)
    _blob = []
    for _root in ("Task", "mcu_bsp", "Core"):
        for _dp, _, _fns in os.walk(os.path.join(BASE, _root)):
            for _fn in _fns:
                if _fn.endswith((".c", ".h")):
                    _blob.append(open(os.path.join(_dp, _fn), encoding="utf-8",
                                      errors="replace").read())
    _blob = "\n".join(_blob)
    # 已登记豁免：无（S1/S2 已落地，FEATURE_DISP_ST7735S_SPI 现在被 disp_port.h/驱动/门禁多处引用）
    _V9_OK = set()
    _zombie = []
    for _f in _feat:
        _n = len(re.findall(r"\b" + _f + r"\b", _blob))
        if (_f not in _V9_OK) and (_n < 3):
            _zombie.append(f"{_f}({_n})")
    check(f"V9: {len(_feat)} 个 FEATURE_* 都至少被 2 处使用（防僵尸宏，豁免已登记）",
          not _zombie, ",".join(_zombie))
    # ---- Q2：函数长度门禁（>=120 行 FAIL / >=80 行 WARN）+ 棘轮（存量豁免只许降不许升）----
    # 为什么这么定：长函数没法被穷举测试（本工程能自动测的全是小函数）。棘轮 = 不许新增违规，
    # 也不搞大爆炸重构：存量逐条登记，谁下次动它谁顺手拆。
    def _func_lens(root):
        out = []
        for _dp, _, _fns in os.walk(os.path.join(BASE, root)):
            for _fn in _fns:
                if not _fn.endswith(".c"):
                    continue
                _p = os.path.join(_dp, _fn)
                _ls = open(_p, encoding="utf-8", errors="replace").read().splitlines()
                _st = None
                for _i, _l in enumerate(_ls):
                    if _st is None:
                        if (re.match(r"^[A-Za-z_][\w \*]*\([^;]*\)\s*$", _l)
                                and not _l.lstrip().startswith(("if", "for", "while", "switch", "return"))):
                            _st = _i
                    elif _l.startswith("}"):
                        out.append((_i - _st + 1, os.path.relpath(_p, BASE).replace("\\", "/"), _st + 1))
                        _st = None
        return out

    # 厂商标例：不参与本门禁（system_* 是 CubeMX 生成；OLED.c 是屏厂例程，彩屏 S1 会整体替换）
    _Q2_VENDOR = ("Core/Src/system_stm32f4xx.c", "mcu_bsp/oled/OLED.c")
    # 棘轮基线（2026-09-19 实测行号:行数）—— 只许降不许升；每条写明"何时拆"
    _Q2_BASE = {
        "Task/Src/CmdRx_Task.c": {452: 479},  # dispatch() 命令分派表：纯线性表，拆它收益小风险大
        "Core/Src/main.c": {112: 197},        # main() 组装根：初始化顺序本身就是它的职责
                                              # 2026-09-19 启动架构重构 +1 行（Disp_Task_Init 调用，
                                              # 屏的上电/初始化搬到 Disp_Task.c；注释已压到 1 行）
        "mcu_bsp/key/key_core.c": {17: 151},  # KeyCore_Step() 手势状态机：纯逻辑，已被宿主测试穷举
        # Task/Src/Oled_Task.c: 原 157 行 Oled_UiDraw() 已在 2026-09-19（S1/S2）按页拆成 ui_page_*，
        # 棘轮条目随之删除（每个页面函数都 < 80 行 → 不再需要豁免；别把这条加回来）
    }
    _over120, _grew, _warn80 = [], [], 0
    for _n, _rel, _ln in _func_lens("Task/Src") + _func_lens("mcu_bsp") + _func_lens("Core/Src"):
        if _rel in _Q2_VENDOR:
            continue
        _base = _Q2_BASE.get(_rel, {}).get(_ln)
        if _base is None:
            # 用"起始行号"做键很脆：文件里任何一个 include/注释的增删都会让键错位，
            # 于是合法的存量豁免会被当成"新增超线"（2026-09-19 实踩：加一行 include 就误报）。
            # 兜底：该文件记录过的最大豁免长度 —— 只要不超过它，仍按存量豁免处理（棘轮语义不变）。
            _max_base = max(_Q2_BASE.get(_rel, {}).values(), default=None)
            if (_max_base is not None) and (_n <= _max_base):
                _base = _max_base
        if _base is not None:
            if _n > _base:
                _grew.append(f"{_rel}:{_ln} {_n}>{_base}")
            continue
        if _n >= 120:
            _over120.append(f"{_rel}:{_ln}({_n})")
        elif _n >= 80:
            _warn80 += 1
    check("Q2: 无超过 120 行的函数（棘轮：4 个存量豁免只许降不许升）",
          not _over120 and not _grew,
          "新增超线:" + ",".join(_over120) + " 豁免变长:" + ",".join(_grew))
    print(f"        提示: {_warn80} 个函数落在 80~119 行（警告线，不阻塞；下次动它们时优先拆）")

    bad = []
    for rel in ("mcu_bsp/key/bsp_key.c", "Task/Src/J8108_Task.c", "Task/Src/Oled_Task.c",
                "mcu_bsp/Motor/motor_8108.c", "Task/Src/Monitor_Task.c",
                "mcu_bsp/disp/disp_sh1106_i2c.c", "mcu_bsp/disp/disp_st7735s_spi.c"):
        for i, ln in enumerate(read(os.path.join(BASE, rel.replace("/", os.sep))).splitlines(), 1):
            if re.search(r"\b(LOG_[DIWE]|printf)\s*\(", ln):
                for lit in re.findall(r'"([^"]*)"', ln):
                    if any(ord(c) > 127 for c in lit):
                        bad.append(f"{rel}:{i}")
    check("日志字符串全 ASCII（英文约定，防乱码）", not bad, "; ".join(bad[:3]))

    for rel in ("mcu_bsp/key/bsp_key.c", "Task/Src/J8108_Task.c", "Task/Src/Monitor_Task.c", "Task/Src/CmdRx_Task.c"):
        t = read(os.path.join(BASE, rel.replace("/", os.sep)))
        n = t.count("xTaskCreate(")
        check(f"{os.path.basename(rel)}: xTaskCreate 均有 pdPASS 检查", n > 0 and t.count("pdPASS") >= n)

    # ---- 需求回归（用户明确要求，防被改回去）----
    oled = read(os.path.join(BASE, "Task", "Src", "Oled_Task.c"))
    check("需求: 行0 覆盖 CAN 四态上屏（INIT FAIL/INIT OK/READY/BUS ERR）",
          all(f"J8108_ST_{s}" in oled for s in ("INIT_FAIL", "INIT_OK", "READY", "BUS_ERR")))
    check("需求: 长按计时条不上屏（bar[] 绘制已移除）", "bar[" not in oled)
    bkey = read(os.path.join(BASE, "mcu_bsp", "key", "bsp_key.h"))
    check("需求: PB2 高有效（KEY_ACTIVE_LEVEL=1u，A 板实测）",
          re.search(r"#define\s+KEY_ACTIVE_LEVEL\s+\(1u\)", bkey) is not None)
    check("需求: 日志只打事件 + 绝不重播旧数值（LINK DOWN 时不打数值）",
          "stale values not printed on purpose" in read(os.path.join(BASE, "Task", "Src", "Monitor_Task.c")))

    # ---- 协议 v1 架构（2026-09-17 定稿）----
    cmd = read(os.path.join(BASE, "Task", "Src", "CmdRx_Task.c"))
    ctrl = read(os.path.join(BASE, "mcu_bsp", "ctrl", "ctrl_core.c"))
    mon = read(os.path.join(BASE, "Task", "Src", "Monitor_Task.c"))
    uia = read(os.path.join(BASE, "mcu_bsp", "Motor", "ui_action.h"))
    cp = read(os.path.join(BASE, "mcu_bsp", "proto", "cmd_parse.h"))
    tx = read(os.path.join(BASE, "mcu_bsp", "proto", "proto_tx.c"))
    mh = read(os.path.join(BASE, "mcu_bsp", "Motor", "motor_8108.h"))
    mc = read(os.path.join(BASE, "mcu_bsp", "Motor", "motor_8108.c"))

    check("协议: 命令表齐全（19 类关键命令）",
          all(f"CMD_{k}" in cp for k in ("PING", "VER", "STAT", "EN", "DIS", "STOP", "ESTOP", "MODE", "DAMP",
                                         "V", "P", "T", "IMP", "HOLD", "SETP", "LIM", "RATE", "WD", "TEL")))
    check("协议: 三类行前缀齐全（@OK/@ERR/@EVT/@TEL）",
          all(p in (cmd + mon + read(os.path.join(BASE, "Task", "Src", "J8108_Task.c"))) for p in
              ("@OK ", "@ERR ", "@EVT ", "@TEL ")))
    check("协议: CmdRx **不直接碰 CAN**（发帧一律交 J8108 任务）",
          not any(k in cmd for k in ("HAL_CAN_", "J8108_SendCmd", "J8108_SendMIT")))
    check("协议: 使能类走异步握手（ReqStart/ReqResult/ReqClear）",
          all(k in cmd for k in ("J8108_ReqStart", "J8108_ReqResult", "J8108_ReqClear")))
    check("协议: ISR 侧只搬字节（无 snprintf/LOG 于中断回调）",
          "void CmdRx_RxIsr(void)" in cmd and "snprintf" not in cmd.split("void CmdRx_RxIsr(void)")[1].split("static uint8_t ring_pop")[0])
    check("协议: 上行串行化（proto_tx 互斥 + 整行 blocking 发送）",
          "xSemaphoreCreateMutex" in tx and "USART_TRANSFER_BLOCKING" in tx)

    check("安全: 心跳超时→阻尼（不失能，防坠）", "Ctrl_Heartbeat" in read(os.path.join(BASE, "Task", "Src", "J8108_Task.c")) and "CTRL_EV_TIMEOUT" in ctrl)
    check("安全: 无 ACK 主动丢帧（防无限重传→总线错误）", "HAL_CAN_AbortTxRequest" in read(os.path.join(BASE, "Task", "Src", "J8108_Task.c")))
    check("安全: 位置模式 Kp/Kd 成对下发（文档警告 Kp≠0&Kd=0 失控）",
          "out->kp = p->kp_pos" in ctrl and "out->kd = p->kd_pos" in ctrl)
    check("安全: 设定点速率限制 + 限幅（slew/clampf）", "slew(" in ctrl and "clampf(" in ctrl)
    check("安全: 保护项齐全（跟随/堵转/过温/电机报错位）",
          all(k in ctrl for k in ("CTRL_EV_FOLLOW", "CTRL_EV_STALL", "CTRL_EV_TEMP", "CTRL_EV_MOTERR")))
    check("安全: #SETP 默认锁死（需解锁）", "setp_unlocked" in ctrl and "locked: use" in cmd)

    check("按键: 策略层纯 UI（ui_action.h 无 CAN/发帧符号）",
          not any(k in uia for k in ("SendMIT", "SendCmd", "HAL_CAN")))
    check("按键: 控制任务不再消费按键事件（按键绝不发帧）",
          "KEY_BIT_HOLD_RELEASE" not in read(os.path.join(BASE, "Task", "Src", "J8108_Task.c")))

    check("屏幕: 4 页定义齐全", all(k in oled for k in ("UI_PAGE_LINK", "UI_PAGE_MOTION", "UI_PAGE_SERIAL")) and "SYSTEM" in oled)
    check("屏幕: 局部刷新（逐行比较，无变化不刷）", "s_last[row]" in oled and "memcmp(" in oled)
    check("Monitor: 三层同源快照（屏/日志/遥测都读 CopySnapshot）",
          "J8108_CopySnapshot" in mon and mon.count("Monitor_FormatTel(b") >= 1)
    check("Monitor: 优先级 3（低于控制 5）且周期 100ms/1000ms",
          "MON_TASK_PRIORITY (3U)" in mon and "MON_UI_MS (100U)" in mon and "MON_LOG_MS (1000U)" in mon)
    check("Monitor: 数值只在数据新鲜时输出", "MON_FRESH_MS" in mon and "fresh != 0u" in mon)
    check("控制: 控制任务优先级 5（保时）+ 200Hz 帧周期", "J8108_TASK_PRIORITY (5U)" in read(os.path.join(BASE, "Task", "Src", "J8108_Task.c")) and "J8108_CTRL_PERIOD_MS (5U)" in read(os.path.join(BASE, "Task", "Src", "J8108_Task.c")))

    check("口径: 双编（输出端、±12.56 rad、不除 8）",
          "J8108_SCALE_OUTPUT_SIDE (1u)" in mh and "J8108_P_HI (12.56f)" in mh)
    check("口径: 反馈 byte0 = ERR 报错位已解算", "s_dev.fb.err = d[0]" in mc)
    # ---- 驱动初始化接线（2026-09-18 实机教训：J8108_Task_Init 漏调 J8108_Init → 状态恒 INIT_FAIL）----
    jt3 = read(os.path.join(BASE, "Task", "Src", "J8108_Task.c"))
    check("接线: J8108_Task_Init 必须调用 J8108_Init(&hcan1)（否则 CAN 从未注册→恒 INIT_FAIL）",
          "J8108_Init(&hcan1);" in jt3)
    check("接线: J8108_Task_Init 先注册 CAN 再建任务",
          jt3.index("J8108_Init(&hcan1);") < jt3.index("xTaskCreate(j8108_task"))
    check("接线: 每个任务模块的 Init 都要真正建任务（pdPASS 可见）",
          "xTaskCreate(" in jt3 and "pdPASS" in jt3)
    # ---- 串口诊断/容错（2026-09-18 实机教训：只发 CR 或不发换行时"看起来没反应"）----
    cx2 = read(os.path.join(BASE, "Task", "Src", "CmdRx_Task.c"))
    check("串口: 行结束符容错（\r 或 \n 都作行结束）", "(ch == '\\n') || (ch == '\\r')" in cx2)
    check("串口: 有原始字节计数 + 前 3 行原样打印（联调诊断）",
          "s_rx_bytes" in cx2 and "CmdRx_RxBytes" in cx2 and "rx line %u" in cx2)
    # ---- 操作手册 ↔ 实现一致性（防文档漂移：改了命令/回包就必须同步改手册）----
    man = read(os.path.join(BASE, "docs", "1_规则（既定事实）/操作手册_串口命令.md"))
    CMDNAMES = ["PING", "VER", "STAT", "LOG", "CLR", "SET", "GET", "EN", "DIS", "STOP", "ESTOP", "ZERO", "MODE",
                "DAMP", "V", "P", "T", "IMP", "HOLD", "SETP", "LIM", "RATE", "WD", "TEL", "HELP"]
    per_parse = [n for n in CMDNAMES if f'"{n}"' not in cp]
    missing_doc = [n for n in CMDNAMES if f"#{n}" not in man]
    check("安全: #STAT 无新鲜数据时不打解码数值（rx=0 全 0 缓冲会解成量程最小值）",
          "values withheld: no fresh frame" in cmd and "link=DOWN mode=" in cmd and "link=UP mode=" in cmd)
    check("安全: 屏 P0 从未收到反馈时显示 AGE never（不是 AGE 0ms）",
          "AGE never  LINK DOWN" in read(os.path.join(BASE, "Task", "Src", "Oled_Task.c")))
    # ---- 崩溃取证与栈裕量（2026-09-18 实机"发完命令板子没动静"后新增）----
    check("诊断: 崩溃黑匣子（RTC 备份寄存器，复位不清 → 开机自报）",
          all(k in read(os.path.join(BASE, "mcu_bsp", "sys_status", "fault_log.c"))
              for k in ("RTC->BKP0R", "FAULTLOG_MAGIC", "HAL_PWR_EnableBkUpAccess"))
          and ("FaultLog_Store(" in read(os.path.join(BASE, "Core", "Src", "stm32f4xx_it.c")))
          and ("FaultLog_StoreStackOverflow" in read(os.path.join(BASE, "Core", "Src", "freertos.c")))
          and ("FaultLog_InitAndReport" in mainc))
    check("诊断: 上次死因开机自报（HardFault PC/LR/CFSR + 栈溢出任务名，免调试器）",
          all(k in mainc for k in ("report_previous_fault", "s_hf_pc", "s_overflow_task")))
    check("诊断: 三个任务自报栈余量（uxTaskGetStackHighWaterMark + stack_probe.h）",
          ("stack_probe_tick" in jt3) and ("stack_probe_tick" in mon) and ("stack_probe_tick" in cmd)
          and ("INCLUDE_uxTaskGetStackHighWaterMark 1" in read(os.path.join(BASE, "Core", "Inc", "FreeRTOSConfig.h"))))
    # 检查"调用形态"而不是关键词（注释里出现 portMAX_DELAY 是说明文字，不算违规）
    check("健壮: Proto_Send 有界等待（不得用 portMAX_DELAY 把协议口永久锁死）",
          ("xSemaphoreTake(s_tx_mtx, pdMS_TO_TICKS(PROTO_TX_WAIT_MS))" in tx)
          and ("xSemaphoreTake(s_tx_mtx, portMAX_DELAY)" not in tx))
    check("健壮: CAN 发送自旋 guard 要短（prio5 任务长自旋会饿死串口/屏幕/LED）",
          ("++guard > 500u" in mc) and ("tx_dropped" in mc))
    check("诊断: 控制循环抖动可见（dtmax 记录 + 10s 摘要打印）",
          ("J8108_LoopJitterTake" in jt3) and ("dtmax=" in mon))
    check("健壮: CmdRx 栈 ≥640 words（snprintf 浮点 + 快照 + b[256] 的实机教训）",
          "CMD_TASK_STACK_WORDS (640U)" in cmd)
    check("诊断: LIMIT 事件带三种来源（setpoint/torque/trate）",
          ('"trate"' in ctrl) and ("CTRL_LIM_TRATE" in read(os.path.join(BASE, "mcu_bsp", "ctrl", "ctrl_core.h"))))
    check("操作手册: 25 个命令与解析表一致且手册全覆盖", (not per_parse) and (not missing_doc),
          f"解析表缺 {per_parse} / 手册缺 {missing_doc}")
    lits_cx = ["mode=DAMP frames=ON", "still enabled", "shaft FREE", "not enabled (send #EN first)",
               "exceeds #LIM T (torque clamp)", "outside soft limits (see #LIM)", "persistent! resend as",
               "usage: #V <deg/s> [kd] [tff]", "TEL period=0 (off)"]
    drift = [l for l in lits_cx if (l not in cmd) or (l not in man)]
    check("操作手册: 关键回包文本与实现逐字一致（防手册过时）", not drift, str(drift))
    keys = ["KD_DAMP", "KP_POS", "KD_POS", "KP_V", "KI_V", "KP_IMP", "KD_IMP", "INPOS", "FOLLOW",
            "RATE", "TRATE", "TFF", "WD", "VMAX", "TMAX", "AUTODAMP", "SETP"]
    k_drift = [k for k in keys if (k not in cmd) or (k not in man)]
    check("操作手册: #SET 键表 17 项与实现一致", not k_drift, str(k_drift))
    check("屏: P2 显示 RX 行数/字节数（区分没字节 vs 没换行）",
          "%luL %luB" in read(os.path.join(BASE, "Task", "Src", "Oled_Task.c")))
    jt2 = read(os.path.join(BASE, "Task", "Src", "J8108_Task.c"))
    check("安全: 运动模式切换前预置 set_applied（防从旧设定点猛冲）",
          all(k in jt2 for k in ("Ctrl_SetMode(&s_ctrl, CTRL_MODE_POS", "Ctrl_SetMode(&s_ctrl, CTRL_MODE_SPEED",
                                 "Ctrl_SetMode(&s_ctrl, CTRL_MODE_TORQUE", "Ctrl_SetMode(&s_ctrl, CTRL_MODE_IMP")))
    check("安全: #ZERO 需二次确认（CONFIRM）", "resend as `#ZERO CONFIRM`" in cmd)
    check("口径: TX 无 ACK 迟滞判据（连续占用，非瞬时）", "J8108_TxStuck" in mc and "s_tx_busy" in mc)


# ============================================================ 构建 + 链接证据
def build_and_link():
    print("---- 固件全量重建（UV4 -r）+ 链接证据 ----")
    if not os.path.exists(UV4):
        check("UV4 可用", False, UV4)
        return
    r = run([UV4, "-r", f"{PROJ}.uvprojx", "-j0", "-t", PROJ, "-o", "rebuild_verify.log"], cwd=MDK)
    log = read(os.path.join(MDK, "rebuild_verify.log"))
    # UV4 退出码：0=无错无警；1=仅有告警。本工程按设计就会有一条告警
    # （关闭电机模块的 R7 编译期告警）→ 两者都算通过；错误数另行断言。
    check("UV4 退出码 0/1（1=仅有告警）", r.returncode in (0, 1), str(r.returncode))
    _sum = [l for l in log.splitlines() if re.search(r"\d+ Error\(s\)", l)]
    check("构建 0 Error（告警允许，逐条打印）",
          bool(_sum) and " 0 Error(s)" in _sum[-1],
          (_sum[-1].strip() if _sum else "no summary"))
    need = ["main.c", "motor_8108.c", "bsp_key.c", "key_core.c", "Oled_Task.c", "J8108_Task.c",
            "ctrl_core.c", "proto_tx.c", "CmdRx_Task.c", "Monitor_Task.c", "disp_geom.c"]
    # 构建日志只在"真的重编"时出现 "compiling X"；增量构建（无源码变化）不会 → 不能据此判失败。
    # 更硬的证据是 **MAP 里的对象文件**（只要参与链接就一定有），故以 MAP 为准。
    _m = read(os.path.join(MDK, PROJ, f"{PROJ}.map"))
    # Keil 的对象文件名为**全小写**（J8108_Task.c -> j8108_task.o）
    missing_obj = [o for o in (f.replace(".c", ".o").lower() for f in need) if o not in _m]
    # 当前默认组合决定"该有哪些对象"，也决定"哪些对象不该出现"（裁剪证据）
    _fc_txt = read(os.path.join(BASE, "Task", "Inc", "feature_config.h"))
    _def_prof = re.search(r"#define\s+CFG_PROFILE\s+(PROFILE_\w+)", _fc_txt).group(1)
    _j8108_on = _def_prof in ("PROFILE_FULL", "PROFILE_MOTOR_DEV")
    if not _j8108_on:
        _dropped = [_o for _o in missing_obj if "j8108" in _o.lower()]
        missing_obj = [_o for _o in missing_obj if "j8108" not in _o.lower()]
        print(f"        当前组合 {_def_prof}：电机模块不参与编译 → 不要求 {_dropped}（这正是裁剪证据）")
    check("当前组合应有的关键文件全部参与链接（MAP 对象证据）", not missing_obj, str(missing_obj))
    compiled = [f for f in need if f"compiling {f}" in log]
    if compiled:
        print(f"        本次构建实际重编 {len(compiled)} 个关键文件；其余为增量复用（对象已在 MAP 内）")
    else:
        print("        本次为增量构建（无文件需重编）；对象存在性由 MAP 断言保证")
    for l in log.splitlines():
        if "Program Size" in l:
            print("        " + l.strip())
    hexf = os.path.join(MDK, PROJ, f"{PROJ}.hex")
    # 构建失败时**不采信任何产物**：旧的 hex/map 还在磁盘上，照打 hash 会让人误以为"产物是新的"
    # （2026-09-19 的真实教训：链接 7 个 L6218E 那轮，门禁自己 traceback 退出，看不到汇总表）。
    _build_ok = bool(_sum) and (" 0 Error(s)" in _sum[-1])
    if not _build_ok:
        check("构建产物 hex 是新构建的（构建失败 → 旧 hex/旧 map 一律不采信）", False,
              "构建未成功，先看上面 Error 行")
    elif not os.path.exists(hexf):
        check("构建产物 hex 存在", False, hexf)
    else:
        print(f"        hex: {os.path.getsize(hexf)}B  sha256={hashlib.sha256(open(hexf,'rb').read()).hexdigest()[:32]}...")

    m = read(os.path.join(MDK, PROJ, f"{PROJ}.map"))
    # ---- 产物新鲜度：hex 必须比**全部源文件+工程文件**新 ----
    # 防的是本项目反复踩过的坑："改了代码却拿旧产物去烧"（版本串/行为都对不上还查不出为什么）。
    _newest, _newest_f = 0.0, ""
    for _root in ("Core", "Task", "mcu_bsp", "Device", "Drivers", "Middlewares"):
        for _dp, _dn, _fns in os.walk(os.path.join(BASE, _root)):
            if "_backup" in _dp:
                continue
            for _fn in _fns:
                if _fn.endswith((".c", ".h", ".s", ".cpp")):
                    _pth = os.path.join(_dp, _fn)
                    try:
                        _mt = os.path.getmtime(_pth)
                    except OSError:
                        continue
                    if _mt > _newest:
                        _newest, _newest_f = _mt, _pth
    _proj_mt = os.path.getmtime(os.path.join(MDK, f"{PROJ}.uvprojx"))
    if _proj_mt > _newest:
        _newest, _newest_f = _proj_mt, os.path.join(MDK, f"{PROJ}.uvprojx")
    if _build_ok and os.path.exists(hexf):
        _hex_mt = os.path.getmtime(hexf)
        check(f"产物新鲜度：hex 比全部源文件新（最新源：{os.path.relpath(_newest_f, BASE)}）",
              _hex_mt > _newest,
              f"hex={_hex_mt:.0f} 源={_newest:.0f} → 改了代码没重建？")
    else:
        check("产物新鲜度：hex 比全部源文件新（构建失败 → 本项无法判定）", False, "先修构建")

    check("MAP: main.o -> Key_Task_Init", "main.o(.text.main) refers to bsp_key.o(.text.Key_Task_Init)" in m)
    if _j8108_on:
        check("MAP: main.o -> J8108_Task_Init", "main.o(.text.main) refers to j8108_task.o(.text.J8108_Task_Init)" in m)
    else:
        check("MAP: 电机模块被裁掉（main.o 不再引用 J8108_Task_Init；注意停用桩对象仍在，是设计如此）",
              "main.o(.text.main) refers to j8108_task.o" not in m)

    check("MAP: main.o -> Monitor_Task_Init", "refers to monitor_task.o(.text.Monitor_Task_Init)" in m)
    check("MAP: main.o -> CmdRx_Task_Init", "refers to cmdrx_task.o(.text.CmdRx_Task_Init)" in m)

    # ---- V5：显示驱动真的进镜像 + 契约绑定成立（换屏后这条绑定必须仍然成立）----
    _ds = ("disp_st7735s_spi" if re.search(r"#define\s+FEATURE_DISP_ST7735S_SPI\s+1u", _fc_txt)
           else "disp_sh1106_i2c")
    check(f"MAP: {_ds}.o 参与链接（当前选中的显示驱动，对象证据）", f"{_ds}.o" in m)
    check(f"MAP: main.o -> {_ds}.o(.text.Disp_Init)（组装根只通过契约绑定驱动）",
          f"main.o(.text.main) refers to {_ds}.o(.text.Disp_Init)" in m)
    check(f"MAP: 页面层 -> {_ds}.o（Oled_Task 调的是 Disp_*，实现落在驱动里）",
          ("oled_task.o(" in m) and (f"refers to {_ds}.o" in m))
    check("MAP: disp_geom.o 参与链接（纯逻辑层，被驱动调用）", "disp_geom.o" in m)


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
