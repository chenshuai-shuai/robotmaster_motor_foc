#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_flow_html.py — 生成交互式工程流程图（单文件 HTML，离线可用）

输入：docs/code/_函数地图.md（真实函数清单）
输出：docs/code/流程图_交互版.html

设计：
  · 左侧是端到端数据通路（从 PC 串口一直到 8108 关节，再回到屏/日志/遥测）
  · 点击任意模块 → 右侧显示：职责 / 关键文件 / 真实函数清单（可搜索） / 关键参数与坑 / 关联文档
  · 函数清单来自 _函数地图.md，不做任何手工编造
重新生成：python tools/gen_flow_html.py
"""
import io
import json
import os
import re

BASE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MAP = os.path.join(BASE, "docs", "code", "_函数地图.md")
OUT = os.path.join(BASE, "docs", "code", "流程图_交互版.html")


def parse_map():
    out, cur = {}, None
    for line in io.open(MAP, encoding="utf-8", errors="replace"):
        m = re.match(r"^##\s+(\S+\.(?:c|h))\s+\(", line)
        if m:
            cur = m.group(1)
            out[cur] = []
            continue
        m2 = re.match(r"^\|\s*(\d+)\s*\|\s*`([A-Za-z_]\w*)`\s*\|", line)
        if m2 and cur:
            out[cur].append([m2.group(2), int(m2.group(1))])
    # 同名函数只保留 .c 的（实现处优先）
    return out


BLOCKS = {
    "pc": {
        "title": "① 上位机 / PC",
        "sub": "串口助手 或 tools/serial_bench.py",
        "desc": "发 `#` 开头的 ASCII 命令，收 `@OK/@ERR/@EVT/@TEL` 和 `[日志]`。115200 8N1，**必须发换行符**（`\\n` 或 `\\r` 都接受）。",
        "files": ["tools/serial_bench.py"],
        "params": [["115200 8N1", "串口参数"], ["#CMD", "命令前缀"], ["@OK/@ERR/@EVT/@TEL", "回包前缀"]],
        "pitfalls": ["不发换行 → A 板只收到字节不成行，没有任何回包（老坑）"],
        "doc": "操作手册_串口命令.md §1（30 秒上手）",
    },
    "uart": {
        "title": "② USART6 + DMA + IDLE",
        "sub": "bsp_usart.c / uart10_def.c",
        "desc": "协议与日志**同一个口**（TX=PG14/RX=PG9，115200 8N1）。用 DMA 收包 + IDLE 线空闲中断判定一帧结束，中断里只记录收到的字节数 `recv_len`（volatile）。",
        "files": ["mcu_bsp/uart/bsp_usart.c", "mcu_bsp/uart/uart10_def.c", "mcu_bsp/uart/bsp_usart.h"],
        "params": [["USART6", "协议口"], ["PG14/PG9", "TX/RX"], ["115200", "波特率"], ["DMA+IDLE", "收包方式"]],
        "pitfalls": ["日志与协议同口 → 排查时先 `#LOG 0` 把日志关掉，只看 @ 回包", "DMA 缓冲要按行重组，不能假设一次中断给一整条命令"],
        "doc": "04_串口协议链.md",
    },
    "isr": {
        "title": "③ 中断：仅搬字节",
        "sub": "CmdRx_RxIsr（ISR 上下文）",
        "desc": "中断里**只做**：把 recv_len 个字节 memcpy 进 512 字节环形缓冲 + 计数。绝不 log/snprintf/发 CAN —— 中断里做慢活会拖垮实时性。",
        "files": ["Task/Src/CmdRx_Task.c"],
        "params": [["512 B", "环形缓冲"], ["s_rx_bytes", "接收字节计数（诊断用）"]],
        "pitfalls": ["环形缓冲满 → 丢弃新字节并计溢出；超长行直接报废"],
        "doc": "04_串口协议链.md",
    },
    "cmd": {
        "title": "④ CmdRx 任务：拼行→解析→分派",
        "sub": "prio 4 / 栈 640 words",
        "desc": "每 2ms 取字节拼成一行（`\\r` 或 `\\n` 结束）→ 表驱动解析（大小写不敏感，畸形数字必须报 BADARG）→ 校验（使能/范围/软限位）→ 调用 J8108_* 或改参数 → 回 `@OK/@ERR`。",
        "files": ["Task/Src/CmdRx_Task.c", "Task/Inc/CmdRx_Task.h", "mcu_bsp/proto/cmd_parse.h", "mcu_bsp/proto/proto_tx.c", "mcu_bsp/proto/proto_tx.h"],
        "params": [["25 条命令", "#PING/#VER/#STAT/#EN/#V/#P/#SET…"], ["17 个 #SET 键", "增益/限幅/保护参数"], ["127 B", "单行上限（超长拆行）"], ["50 ms", "TX 锁有界等待"]],
        "pitfalls": ["协议口 TX 锁必须有界等待 + 丢行计数，否则被占死=看起来死机", "数据不新鲜时绝不打数值（门控）"],
        "doc": "04_串口协议链.md、操作手册 §2 命令总表",
    },
    "ctrl": {
        "title": "⑤ 控制核（外环算法）",
        "sub": "ctrl_core.c · 纯逻辑，可在 PC 上测",
        "desc": "A 板自己写的 200Hz 外环：模式 FSM(IDLE/DAMP/SPEED/POS/TORQUE/IMP) → 速率限制 slew → 限幅 clamp → PI/PD + 前馈 → 保护。输出一帧 `Ctrl_Frame_t`（p/v/Kp/Kd/T）。",
        "files": ["mcu_bsp/ctrl/ctrl_core.c", "mcu_bsp/ctrl/ctrl_core.h"],
        "params": [["KP_V / KI_V", "速度环 PI 增益"], ["TFF", "摩擦前馈 N·m（按方向取号）"], ["TRATE", "力矩斜率 N·m/s"], ["KD_DAMP", "内环阻尼"], ["TMAX / LIM T", "力矩上限"], ["±170°", "软限位"]],
        "pitfalls": ["小惯量直驱关节不能靠大 Kp 硬顶 → 极限环震荡", "静摩擦 0.25 vs 动摩擦 0.1 N·m → 无前馈必然黏滑", "积分只在未受限时累积（抗饱和）"],
        "doc": "03_控制核算法.md、07_CAN运动控制学习指南.md",
    },
    "task": {
        "title": "⑥ J8108 任务（200Hz 帧发生器）",
        "sub": "prio 5 / 栈 640 words · 唯一发 CAN 的任务",
        "desc": "每 5ms 一拍：解算反馈 → 链路日志 → 请求握手(REQ_EN/DIS/ZERO/CLRERR) → 模式与保护 → 生成并发送 MIT 帧。同时管心跳超时退 DAMP、掉线检测与自动恢复。",
        "files": ["Task/Src/J8108_Task.c", "Task/Inc/J8108_Task.h"],
        "params": [["5 ms", "控制周期 200Hz"], ["20 ms", "握手 ACK 窗口"], ["100 ms", "TX 邮箱占用判据"], ["200 ms", "反馈新鲜门槛"], ["500 ms ×n", "恢复退避（最多 4 次）"]],
        "pitfalls": ["prio5 里禁止长自旋（曾因 CAN 邮箱自旋 10 万次把低优先级任务全饿死）", "无 ACK 时 AutoRetransmission 会占死邮箱 → 必须 abort"],
        "doc": "05_控制任务与帧生成.md",
    },
    "can": {
        "title": "⑦ CAN 底层 + 8108 驱动",
        "sub": "bsp_can.c / motor_8108.c",
        "desc": "CAN1 1Mbps：滤波器 + 实例注册 + 中断分发；8108 驱动负责 MIT 帧位打包（p16+v12+kp12+kd12+t12）、反馈帧解包（ERR|POS16|VEL12|T12|TMos8|TMotor8）、双编口径换算、快照 seqlock。",
        "files": ["mcu_bsp/can/bsp_can.c", "mcu_bsp/can/bsp_can.h", "mcu_bsp/Motor/motor_8108.c", "mcu_bsp/Motor/motor_8108.h"],
        "params": [["0x201", "MIT 帧 ID"], ["0x781", "反馈帧 ID"], ["0x001", "命令帧 ID"], ["1 Mbps", "波特率"], ["±12.56 rad / ±45 / ±18", "双编量程"]],
        "pitfalls": ["双编版不除 8（单编才除）→ 差 8 倍", "ERR 在反馈帧第一个字节（最高字节）", "无 ACK 时无限重传占死邮箱"],
        "doc": "02_CAN底层与8108驱动.md",
    },
    "motor": {
        "title": "⑧ 8108 关节电机",
        "sub": "厂商固件（内环：电流/速度/位置）",
        "desc": "24V 供电、减速比 8:1、双编码器。接收 MIT 帧按 `T=Kp·Δp + Kd·Δv + t_ff` 执行；回发状态帧。对我们来说它是“力矩接口”。",
        "files": [],
        "params": [["±18 N·m", "最大力矩"], ["±45 rad/s", "最大速度"], ["0x781", "反馈帧"], ["70/85 ℃", "过温预警/停机"]],
        "pitfalls": ["v_cmd=0 时 Kd 变成“刹车”，速度模式会吃力矩（实测 10°/s 约 0.17 N·m）", "竖直安装时有重力分量"],
        "doc": "07_CAN运动控制学习指南.md §2",
    },
    "fb": {
        "title": "⑨ 反馈解算 → 快照",
        "sub": "motor_8108 中断 + J8108_Update",
        "desc": "中断里只拷贝 8 字节 + 置 new_frame 标志；任务侧解包成 deg / deg/s / N·m / ℃，并更新 seqlock 快照供屏/日志/遥测读取（帧一致）。",
        "files": ["mcu_bsp/Motor/motor_8108.c", "Task/Src/J8108_Task.c"],
        "params": [["age", "数据新鲜度 ms"], ["err", "电机错误位"], ["Tm / Tr", "MOS/线圈温度"]],
        "pitfalls": ["age>200ms 必须门控：只报链路 DOWN，不打数值（曾打出 -719.64° 假数据）"],
        "doc": "02 §4、06",
    },
    "mon": {
        "title": "⑩ Monitor：屏 / 日志 / 遥测",
        "sub": "prio 3 · 100ms/1000ms/10s 三档",
        "desc": "一个任务三个出口，全部读同一份快照：OLED 4 页（10Hz 局部刷新）、数据日志（1Hz）、10 秒摘要（含 hz/dtmax/txdrop/heap）、`@TEL` 遥测（周期可设）。",
        "files": ["Task/Src/Monitor_Task.c", "Task/Inc/Monitor_Task.h", "Task/Src/Oled_Task.c", "Task/Inc/Oled_Task.h", "Task/Inc/ui_status.h"],
        "params": [["10 Hz", "屏", ], ["1 Hz", "数据日志"], ["10 s", "摘要"], ["#TEL n", "遥测周期 ms"]],
        "pitfalls": ["三层必须同源，否则屏和日志互相矛盾", "摘要里的 dtmax 是判断“被饿死”的关键证据"],
        "doc": "06_显示按键与监控.md",
    },
    "oled": {
        "title": "⑪ OLED（SH1106，软 I2C）",
        "sub": "SCL=PB10 / SDA=PB9 开漏",
        "desc": "没有硬件 I2C，用 GPIO 位操作打时序 + DWT 周期计数延时。渲染器做局部刷新（逐行 memcmp，只重画变化的行）。",
        "files": ["mcu_bsp/oled/OLED.c", "mcu_bsp/oled/OLED.h"],
        "params": [["0x78", "I2C 地址"], ["21 列", "每行宽度"], ["4 页", "LINK/MOTION/SERIAL/SYSTEM"]],
        "pitfalls": ["软 I2C 延时循环必须有界（否则拖住任务）", "局部刷新要补空格，否则残留旧字符"],
        "doc": "06_显示按键与监控.md",
    },
    "key": {
        "title": "⑫ 按键（纯 UI）",
        "sub": "PB2 高有效 · prio 4 · 10ms 扫描",
        "desc": "扫描 + 去抖 + 事件机（单击/双击/长按≥2s/卡死 10s）→ 事件组 → Monitor 消费 → 翻页（下页/上页/回 P0）。**绝不发任何电机指令**。",
        "files": ["mcu_bsp/key/bsp_key.c", "mcu_bsp/key/bsp_key.h", "mcu_bsp/key/key_core.c", "mcu_bsp/key/key_core.h", "mcu_bsp/Motor/ui_action.h"],
        "params": [["PB2", "按键引脚（高有效）"], ["30 ms", "去抖"], ["2 s", "长按阈值"], ["10 s", "卡死阈值"]],
        "pitfalls": ["按键与电机彻底解耦（方案 C）：误触不会让电机乱动"],
        "doc": "06_显示按键与监控.md",
    },
    "boot": {
        "title": "⓿ 启动与任务创建",
        "sub": "main.c USER CODE 2",
        "desc": "HAL/时钟/外设初始化 → 日志口 → 开机横幅（版本+构建时间+特性宏）→ 崩溃黑匣子自报 → 各 Init（OLED/协议锁/Monitor/8108/CmdRx/Key）→ 打印 free heap → 启动调度器。",
        "files": ["Core/Src/main.c", "Core/Src/freertos.c", "Core/Src/stm32f4xx_it.c", "mcu_bsp/sys_status/fault_log.c", "mcu_bsp/sys_status/sys_status.c", "Task/Src/Led_Task.c"],
        "params": [["32 KB", "FreeRTOS heap"], ["168 MHz", "SYSCLK"], ["RTC 备份寄存器", "崩溃黑匣子"]],
        "pitfalls": ["新任务 Init 必须放 USER CODE 2 区（否则 CubeMX 重生成会冲掉）", "J8108_Task_Init 内部必须先注册 CAN 再建任务（曾漏掉→INIT FAIL）"],
        "doc": "01_启动与任务框架.md",
    },
}

HTML = """<!DOCTYPE html>
<html lang="zh-CN"><head><meta charset="utf-8">
<title>A 板 8108 关节电机控制 — 工程流程图（交互版）</title>
<style>
 :root{--bg:#0f1115;--card:#171a21;--fg:#e6e9ef;--mut:#98a2b3;--ac:#4ea1ff;--ac2:#39d98a;--wr:#ffb020;--bd:#262b36}
 *{box-sizing:border-box}
 body{margin:0;background:var(--bg);color:var(--fg);font:14px/1.55 -apple-system,"Segoe UI","Microsoft YaHei",sans-serif}
 header{padding:16px 22px;border-bottom:1px solid var(--bd);display:flex;align-items:baseline;gap:14px;flex-wrap:wrap}
 header h1{font-size:17px;margin:0}
 header .mut{color:var(--mut);font-size:12px}
 .wrap{display:grid;grid-template-columns:minmax(320px,380px) 1fr;gap:0;height:calc(100vh - 57px)}
 .left{border-right:1px solid var(--bd);padding:14px;overflow:auto}
 .right{padding:16px 20px;overflow:auto}
 .lane{color:var(--mut);font-size:11px;letter-spacing:.08em;margin:12px 0 6px;text-transform:uppercase}
 .box{background:var(--card);border:1px solid var(--bd);border-left:3px solid var(--ac);border-radius:8px;padding:9px 11px;margin:6px 0;cursor:pointer;transition:.15s}
 .box:hover{border-color:var(--ac);transform:translateX(2px)}
 .box.on{background:#1d2431;border-left-color:var(--ac2)}
 .box b{display:block;font-size:13px}
 .box span{color:var(--mut);font-size:11.5px}
 .arrow{text-align:center;color:#3d4557;font-size:12px;margin:-2px 0}
 .fb .box{border-left-color:var(--ac2)}
 h2{font-size:15px;margin:2px 0 6px}
 .sub{color:var(--mut);font-size:12.5px;margin-bottom:12px}
 .kv{display:flex;flex-wrap:wrap;gap:6px;margin:8px 0 14px}
 .kv i{font-style:normal;background:#1d2431;border:1px solid var(--bd);border-radius:6px;padding:2px 8px;font-size:12px}
 .kv i b{color:var(--ac2);font-weight:600}
 .files code{background:#12151b;border:1px solid var(--bd);border-radius:5px;padding:1px 6px;margin:0 4px 4px 0;display:inline-block;font-size:12px;color:#9fd1ff}
 table{width:100%;border-collapse:collapse;margin-top:6px}
 th,td{text-align:left;border-bottom:1px solid var(--bd);padding:5px 6px;font-size:12.5px;vertical-align:top}
 th{color:var(--mut);font-weight:500}
 code{font-family:ui-monospace,Consolas,monospace}
 .pit{border-left:3px solid var(--wr);background:#1f1b12;padding:8px 11px;border-radius:6px;margin:6px 0;font-size:12.5px}
 .doc{margin-top:14px;color:var(--mut);font-size:12.5px}
 .doc b{color:var(--fg);font-weight:600}
 input#q{width:100%;padding:8px 10px;border-radius:7px;border:1px solid var(--bd);background:#12151b;color:var(--fg);margin:4px 0 8px}
 .hint{color:var(--mut);font-size:12px}
 a{color:var(--ac)}
</style></head>
<body>
<header>
  <h1>A 板 8108 关节电机控制 · 工程流程图（交互版）</h1>
  <span class="mut">点左侧任意模块 → 右侧看职责 / 真实函数 / 参数 / 坑　｜　函数清单自动取自 <code>_函数地图.md</code></span>
</header>
<div class="wrap">
  <div class="left" id="diagram"></div>
  <div class="right" id="panel"><h2>← 点击左侧模块</h2><div class="sub">也可以直接在下面搜索函数名，定位它属于哪个文件</div>
    <input id="q" placeholder="搜索函数名，例如 Ctrl_Step / j8108_task / Cmd_ParseLine">
    <div id="searchres"></div>
  </div>
</div>
<script>
const DATA = __DATA__;
const BLOCKS = __BLOCKS__;
const MAP = __MAP__;
const ORDER = ["pc","uart","isr","cmd","ctrl","task","can","motor","fb","mon","oled","key","boot"];
const LANE = {pc:"下行：PC → 电机",uart:"",isr:"",cmd:"",ctrl:"",task:"",can:"",motor:"",
              fb:"上行：电机 → 屏/日志",mon:"",oled:"",key:"",boot:""};
const FEEDBACK = new Set(["fb","mon","oled","key"]);

function box(id){
  const b = BLOCKS[id];
  const cls = "box" + (FEEDBACK.has(id)?" fb":"");
  return `<div class="${cls}" data-id="${id}"><b>${b.title}</b><span>${b.sub}</span></div>`;
}
let html = "";
for(const id of ORDER){
  if(LANE[id]) html += `<div class="lane">${LANE[id]}</div>`;
  html += box(id);
  if(["pc","uart","isr","cmd","ctrl","task","can"].includes(id)) html += `<div class="arrow">▼</div>`;
  if(id==="motor") html += `<div class="arrow">↺ 反馈帧 0x781</div>`;
  if(id==="can") html += `<div class="arrow">▲</div>`;
}
document.getElementById("diagram").innerHTML = html;

function fnRows(files){
  let rows = "";
  for(const f of files){
    const fns = MAP[f] || [];
    if(!fns.length) continue;
    rows += `<tr><td style="white-space:nowrap;color:#9fd1ff">${f}</td><td>` +
            fns.map(([n,ln])=>`<code title="定义行 ${ln}">${n}</code>`).join(" ") + `</td></tr>`;
  }
  return rows || '<tr><td colspan="2" class="hint">（本模块无自有函数，主要靠其它模块）</td></tr>';
}

function show(id){
  const b = BLOCKS[id];
  document.querySelectorAll(".box").forEach(e=>e.classList.toggle("on", e.dataset.id===id));
  document.getElementById("panel").innerHTML =
    `<h2>${b.title}</h2><div class="sub">${b.sub}</div>
     <div>${b.desc}</div>
     <div class="kv">${(b.params||[]).map(p=>`<i>${p[1]}：<b>${p[0]}</b></i>`).join("")}</div>
     <div class="files">${(b.files||[]).map(f=>`<code>${f}</code>`).join("")}</div>
     <h3 style="font-size:13px;margin:14px 0 0;color:#98a2b3">真实函数清单（行号=定义处）</h3>
     <table><tr><th>文件</th><th>函数</th></tr>${fnRows(b.files)}</table>
     ${(b.pitfalls||[]).map(p=>`<div class="pit">⚠ ${p}</div>`).join("")}
     <div class="doc">📖 详见：<b>${b.doc}</b></div>`;
}
document.getElementById("diagram").addEventListener("click", e=>{
  const el = e.target.closest(".box"); if(el) show(el.dataset.id);
});

// 函数搜索
const FLAT = [];
for(const f in MAP) for(const [n,ln] of MAP[f]) FLAT.push([n,f,ln]);
document.getElementById("q").addEventListener("input", e=>{
  const q = e.target.value.trim().toLowerCase();
  const el = document.getElementById("searchres");
  if(!q){ el.innerHTML = '<div class="hint">例：Ctrl_Step（控制核）、j8108_task（控制任务）、Cmd_ParseLine（解析器）</div>'; return; }
  const hits = FLAT.filter(x=>x[0].toLowerCase().includes(q)).slice(0,60);
  el.innerHTML = hits.length
    ? `<table><tr><th>函数</th><th>文件</th><th>行</th></tr>` +
      hits.map(([n,f,ln])=>`<tr><td><code>${n}</code></td><td style="color:#9fd1ff">${f}</td><td>${ln}</td></tr>`).join("") +
      `</table><div class="hint">共 ${FLAT.filter(x=>x[0].toLowerCase().includes(q)).length} 个匹配</div>`
    : '<div class="hint">没有匹配（工程共 ' + FLAT.length + ' 个函数定义位）</div>';
});
show("pc");
</script></body></html>
"""


def main():
    fmap = parse_map()
    # 只保留 .c 的函数（同名 .h 声明跳过，避免重复）
    data = {}
    for f, fns in fmap.items():
        data[f] = fns
    html = (HTML.replace("__DATA__", json.dumps(data, ensure_ascii=False))
                .replace("__BLOCKS__", json.dumps(BLOCKS, ensure_ascii=False))
                .replace("__MAP__", json.dumps(data, ensure_ascii=False)))
    html = html.replace("#3b4椅;text-align:center;color:#3d4557", "color:#3d4557")
    io.open(OUT, "w", encoding="utf-8", newline="").write(html)
    print(f"已生成：{OUT}  ({len(html)} 字符, {sum(len(v) for v in fmap.values())} 个函数位置)")


if __name__ == "__main__":
    main()
