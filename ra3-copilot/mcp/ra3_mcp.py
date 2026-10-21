#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""ra3_mcp.py - Ra3-Copilot MCP 服务器（脑机接口）

把红警3 日冕修改器的进程内调试接口（http://127.0.0.1:17832）包装成标准 MCP 工具，
让任意支持 MCP 的 AI 客户端（Claude Desktop / 各类 agent）能够：
  · 读取战场态势（我方/敌方部队与建筑数量、敌人阵营与构成、资金、选中单位）
  · 检索单位目录（中文名 / 拼音 / 英文模板名 / 哈希）
  · 召唤单位、空投箱子、收编/摧毁/满级/复制、调速改血、功能开关

设计：**零依赖**（只用 Python 标准库），MCP stdio 传输（换行分隔的 JSON-RPC 2.0）。
  日志一律走 stderr —— stdout 只允许出现协议消息。

用法（MCP 客户端配置）：
  {
    "mcpServers": {
      "ra3-copilot": { "command": "python", "args": ["<本文件绝对路径>"] }
    }
  }

环境变量：
  RA3_API        调试接口地址（默认 http://127.0.0.1:17832）
  RA3_MCP_ALLOW_RAW=0   关闭 raw（默认开放：raw 里 op=get/post 可调任意 HTTP 路由，
                        另有 peek/read/poke/call 直接读写内存、按 RVA 调函数）
"""
import json
import os
import re
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

_CLS_NAME2IDX = {"其他": 0, "步兵": 1, "载具": 2, "空军": 3, "海军": 4, "建筑": 5, "箱子": 6}
_CAT_NAME2IDX = {"unit": 0, "struct": 1, "other": 2, "单位": 0, "建筑": 1, "其他": 2}

API = os.environ.get("RA3_API", "http://127.0.0.1:17832").rstrip("/")
ALLOW_RAW = os.environ.get("RA3_MCP_ALLOW_RAW", "1") != "0"
SERVER_NAME = "ra3-copilot"
SERVER_VERSION = "0.1.0"
DEFAULT_PROTOCOL = "2024-11-05"

HERE = os.path.dirname(os.path.abspath(__file__))
CATALOG_PATH = os.path.join(HERE, "unit_catalog.json")


def log(*a):
    print("[ra3-mcp]", *a, file=sys.stderr, flush=True)


# ============================ HTTP 桥 ============================
def http(path, timeout=8.0):
    """GET 调试接口；返回 (ok, text)。"""
    url = API + path
    req = urllib.request.Request(url)
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))   # 绕开系统代理
    try:
        with opener.open(req, timeout=timeout) as r:
            return True, r.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as e:
        return False, "HTTP %d: %s" % (e.code, e.read().decode("utf-8", "replace"))
    except Exception as e:
        return False, "无法连接修改器接口（%s）—— 游戏是否在运行且已注入？接口 %s" % (e, API)


def api_get(path):
    ok, txt = http(path)
    if not ok:
        raise RuntimeError(txt)
    return txt


def api_post(path):
    """变更类：带 ai=1 标记（仅作遥测计数；DLL 侧已无闸门，任何请求都放行）。"""
    sep = "&" if "?" in path else "?"
    ok, txt = http(path + sep + "ai=1")
    if not ok:
        raise RuntimeError(txt)
    return txt


# ============================ 单位目录 ============================
class Catalog:
    def __init__(self):
        self.units = []
        self.crates = []
        self.by_hash = {}
        self.by_id = {}
        self._load()

    def _load(self):
        if not os.path.exists(CATALOG_PATH):
            log("未找到目录 %s —— 只能按哈希操作" % CATALOG_PATH)
            return
        with open(CATALOG_PATH, encoding="utf-8") as f:
            doc = json.load(f)
        self.units = doc.get("units", [])
        self.crates = doc.get("crates", [])
        for u in self.units + self.crates:
            self.by_hash[u["h"].lower()] = u
            self.by_id[u["i"].lower()] = u
        log("目录加载：单位 %d / 箱子 %d" % (len(self.units), len(self.crates)))

    def resolve(self, text, crates_only=False):
        """名字 / 模板名 / 哈希 → 目录项。找不到返回 None。"""
        if not text:
            return None
        s = _norm_query(text.strip())
        pool = self.crates if crates_only else (self.units + self.crates)
        # ① 哈希
        if s.lower().startswith("0x") or (len(s) == 8 and all(c in "0123456789abcdefABCDEF" for c in s)):
            h = s if s.lower().startswith("0x") else "0x" + s
            return self.by_hash.get(h.lower())
        # ② 模板名（精确、忽略大小写）
        hit = self.by_id.get(s.lower())
        if hit and (not crates_only or hit in self.crates):
            return hit
        # ③ 显示名精确
        for u in pool:
            if u["n"] == s:
                return u
        # ④ 子串（显示名/模板名/拼音）
        sl = s.lower()
        cands = [u for u in pool if sl in u["n"].lower() or sl in u["i"].lower()
                 or sl in u.get("p", "").lower()]
        if cands:
            # 优先可召唤、名字短的
            cands.sort(key=lambda u: (0 if u.get("m") else 1, len(u["n"])))
            return cands[0]
        return None

    def search(self, query, limit=20, crates_only=False, summonable_only=False):
        pool = self.crates if crates_only else (self.units + self.crates)
        q = _norm_query((query or "").strip())
        out = []
        for u in pool:
            if summonable_only and not u.get("m") and u not in self.crates:
                continue
            if not q:
                out.append(u)
                continue
            ql = q.lower()
            score = None
            if u["n"] == q or u["i"].lower() == ql or u["h"].lower() == ql:
                score = 0
            elif u["n"].lower().startswith(q) or u["i"].lower().startswith(ql):
                score = 1
            elif ql in u["n"].lower() or ql in u["i"].lower():
                score = 2
            elif ql in u.get("p", "").lower():
                score = 3
            if score is not None:
                out.append((score, u))
        if out and isinstance(out[0], tuple):
            out.sort(key=lambda t: (t[0], 0 if t[1].get("m") else 1, len(t[1]["n"])))
            out = [u for _, u in out]
        return out[:max(1, min(int(limit), 100))]


CAT = Catalog()

# 常见叫法与官方译名的差异（玩家习惯写法 → 表里的写法）
_ALIAS = {
    "磁暴": "磁爆",      # 磁爆坦克
    "光棱": "光棱",      # RA3 官方作「光棱坦克」
    "天启": "天启",
    "双刃": "双刃",
    "无畏": "无畏",
    "犀牛": "犀牛",
}


def _norm_query(q):
    for a, b in _ALIAS.items():
        q = q.replace(a, b)
    return q


# ============================ 工具实现 ============================
def t_list_objects(a):
    """列出活体对象（带地址/坐标/血量/AI 指针）—— 自动感知，无需人手点选。"""
    qs = ["limit=%d" % max(1, min(int(a.get("limit", 40)), 100))]
    if a.get("fresh"):
        qs.append("fresh=1")
    q = a.get("query")
    if q:
        qs.append("q=" + urllib.parse.quote(str(q)))
    owner = a.get("owner")
    if owner is not None and int(owner) >= 0:
        qs.append("owner=%d" % int(owner))
    cls = a.get("cls")
    if cls is not None:
        c = _CLS_NAME2IDX.get(str(cls), None)
        c = int(cls) if c is None and str(cls).lstrip("-").isdigit() else c
        if c is not None and int(c) >= 0:
            qs.append("cls=%d" % int(c))
    cat = a.get("cat")
    if cat is not None:
        c = _CAT_NAME2IDX.get(str(cat), None)
        c = int(cat) if c is None and str(cat).lstrip("-").isdigit() else c
        if c is not None and int(c) >= 0:
            qs.append("cat=%d" % int(c))
    raw = api_get("/objects?" + "&".join(qs))
    if a.get("include_unknown"):          # 默认过滤掉场景物/模块子对象/空槽
        return raw
    try:
        d = json.loads(raw)
        objs = [o for o in d.get("objects", [])
                if o.get("name") not in (None, "?", "")
                and not (float(o.get("hpMax") or 0) == 0 and float(o.get("x") or 0) == 0
                         and float(o.get("y") or 0) == 0)]
        d["objects"] = objs
        d["count"] = len(objs)
        d["note"] = "已过滤场景物/模块子对象（要看全部：include_unknown=true）"
        return json.dumps(d, ensure_ascii=False)
    except Exception:
        return raw


def t_object_detail(a):
    """查单个对象的完整状态（Thing / AI / 血量体地址、坐标、血量、归属）。"""
    addr = a.get("addr") or a.get("thing")
    if not addr:
        return "缺少 addr（Thing 地址，见 list_objects 的 thing 字段）"
    return api_get("/obj?addr=" + urllib.parse.quote(str(addr)))


def t_command_unit(a):
    """【指挥】命令某个单位行动（AIUpdate 直调）。
       attack：攻击目标；move：走向世界坐标；idle：待机（占位）。"""
    unit = a.get("unit") or a.get("thing")
    if not unit:
        return "缺少 unit（单位 Thing 地址，见 list_objects 的 thing 字段）"
    act = str(a.get("act") or a.get("action") or "attack").strip().lower()
    if act in ("attack", "atk", "攻击"):
        target = a.get("target")
        if not target:
            return "attack 需要 target（目标 Thing 地址，见 list_objects）"
        r = api_post("/order?unit=%s&act=attack&target=%s" % (unit, target))
        return "命令单位 %s 攻击 %s → %s" % (unit, target, r)
    if act in ("move", "moveto", "移动"):
        x, y, z = a.get("x"), a.get("y"), a.get("z")
        if x is None or y is None:
            return "move 需要 x 与 y（世界坐标；z 可省，默认 0）"
        z = 0 if z is None else z
        r = api_post("/order?unit=%s&act=move&x=%s&y=%s&z=%s" % (unit, x, y, z))
        return "命令单位 %s 移动到 (%.1f, %.1f, %.1f) → %s" % (unit, float(x), float(y), float(z), r)
    if act in ("idle", "stop", "待机", "停止"):
        return "idle → " + api_post("/order?unit=%s&act=idle" % unit)
    return "act 需为 attack / move / idle"


def t_build_structure(a):
    """【建造】把一座建筑直接造出来（建筑有对象代码，用召唤实现）。
       属『直接建造』——建筑立刻完整出现，不走游戏建造队列，也不检查科技前置/资源。
       落点用 place 指定（默认视野中心）；也可给 x/y/z 指定世界坐标。"""
    unit = a.get("unit") or a.get("structure") or a.get("name") or a.get("hash")
    if not unit:
        return "缺少 unit（建筑名 / 模板名 / 哈希；search_units 可加 cls='建筑' 检索）"
    u = CAT.resolve(str(unit))
    if not u:
        return "目录里找不到「%s」。用 search_units(cls='建筑') 查准确名字，或直接给 8 位哈希。" % unit
    x, y = a.get("x"), a.get("y")
    pos = ""
    if x is not None and y is not None:
        z = 0 if a.get("z") is None else a.get("z")
        pos = "&x=%s&y=%s&z=%s" % (x, y, z)
    owner = int(a.get("owner", 0) or 0)
    extra = "&owner=%d" % owner if owner > 0 else ""
    r = api_post("/summon?hash=%s&count=1%s%s%s" % (u["h"], extra, _place_qs(a), pos))
    who = "己方" if owner <= 0 else "玩家#%d" % owner
    return "建造 %s → %s（归属：%s）" % (u["n"], r, who)


def t_game_state(_):
    return api_get("/snapshot")


def t_enemy_report(_):
    return api_get("/enemy")


def t_status(_):
    st = api_get("/ai/status")
    ok, ping = http("/ping")
    return "接口 %s  ping=%s\n%s" % (API, "pong" if ping.strip() == "pong" else "无响应", st)


def t_search_units(a):
    q = a.get("query", "")
    crates = bool(a.get("crates_only"))
    summ = a.get("summonable_only", True)
    hits = CAT.search(q, a.get("limit", 20), crates_only=crates, summonable_only=summ)
    if not hits:
        return "没有匹配「%s」的单位。试试更短的关键词，或先用 get_battle_state 看敌方用了什么。" % q
    lines = ["找到 %d 个（%s）：" % (len(hits), "箱子" if crates else "单位")]
    for u in hits:
        lines.append("  %-16s %-34s %s  [%s%s]%s"
                     % (u["n"], u["i"], u["h"], u["s"], "/" + u["c"] if u.get("c") else "",
                        "" if u.get("m") else " (仅观赏)"))
    return "\n".join(lines)


_PLACE_ALIAS = {
    "view": "view", "vision": "view", "视野": "view", "视野中心": "view",
    "selection": "selection", "selected": "selection", "选中": "selection", "选中单位": "selection",
    "cursor": "cursor", "mouse": "cursor", "光标": "cursor", "鼠标": "cursor",
}


def _place_qs(a):
    """落点来源 -> '&place=xxx'（不认识的名字忽略，交给修改器的落点设置兜底）"""
    p = _PLACE_ALIAS.get(str(a.get("place") or "").strip().lower())
    return ("&place=" + p) if p else ""


def t_summon(a):
    unit = a.get("unit") or a.get("name") or a.get("hash")
    if not unit:
        return "缺少 unit（单位名 / 模板名 / 哈希）"
    u = CAT.resolve(str(unit))
    if not u:
        return "目录里找不到「%s」。用 search_units 查准确名字；也可以直接给 8 位哈希。" % unit
    count = int(a.get("count", 1))
    count = max(1, min(count, 30))
    owner = int(a.get("owner", 0) or 0)          # 0/缺省 = 本地玩家；>0 = 该玩家索引（见 get_battle_state）
    extra = "&owner=%d" % owner if owner > 0 else ""
    who = "己方" if owner <= 0 else "玩家#%d" % owner
    pos = ""
    if a.get("x") is not None and a.get("y") is not None:
        z = 0 if a.get("z") is None else a.get("z")
        pos = "&x=%s&y=%s&z=%s" % (a["x"], a["y"], z)
    r = api_post("/summon?hash=%s&count=%d%s%s%s" % (u["h"], count, extra, _place_qs(a), pos))
    return "召唤 %s ×%d → %s（归属：%s）" % (u["n"], count, r, who)


def t_drop_crate(a):
    kind = a.get("kind") or a.get("crate") or "随机"
    u = CAT.resolve(str(kind), crates_only=True)
    if not u:
        return "没有这个箱子「%s」。用 search_units(crates_only=true) 看可选箱子（治疗/现金/晋升/各类属性箱）。" % kind
    count = max(1, min(int(a.get("count", 1)), 30))
    r = api_post("/summon?hash=%s&count=%d%s" % (u["h"], count, _place_qs(a)))
    return "空投 %s ×%d → %s" % (u["n"], count, r)


# 效果别名 → 箱子模板名。只有「炸弹箱」系列走死亡开火；属性箱（铁幕/加速/护甲…）的
# 效果在拾取时 applyModifier，不在死亡链上，所以不列在这里。
_BURST_EFFECTS = {
    "freeze": "ForzenCrate", "frozen": "ForzenCrate", "冰冻": "ForzenCrate", "冻结": "ForzenCrate",
    "bomb": "BombCrate", "炸弹": "BombCrate",
    "superbomb": "SuperBombCrate", "超级炸弹": "SuperBombCrate",
    "basebomb": "BaseBombCrate", "基地炸弹": "BaseBombCrate",
    "vacuum": "VacuumBombCrate", "真空弹": "VacuumBombCrate",
    "particle": "ParticleCannonCrate", "粒子炮": "ParticleCannonCrate",
    "radar": "RadarCloseBombCrate", "雷达干扰": "RadarCloseBombCrate",
    "metal": "SuperMetaImpactCrate", "金属冲击": "SuperMetaImpactCrate",
    "telekinetic": "TelekineticBombCrate", "念力": "TelekineticBombCrate",
    "emotion": "EmotionDissidentBombCrate", "煽动叛变": "EmotionDissidentBombCrate",
    "nano": "NanoPointDefenceCrate", "纳米防御": "NanoPointDefenceCrate",
    "heal": "HealCrate", "治疗": "HealCrate",
    "money": "MoneyCrate", "现金": "MoneyCrate",
}


def t_burst_crate(a):
    """空投箱子并引爆。

    优先走 DLL 原生 /burst：创建后下一帧由渲染线程直接击杀，全程一条请求，
    箱子最多被画一帧（MCP 层「/summon + /obj + /aicall」至少三帧，肉眼是闪一下）。
    老 DLL 不认识 /burst 时退回旧路径（/summon → 校验 → /aicall）。
    """
    kind = a.get("effect") or a.get("kind") or a.get("crate") or "freeze"
    key = str(kind).strip()
    target = _BURST_EFFECTS.get(key.lower(), key)
    u = CAT.resolve(target, crates_only=True)
    if not u:
        return ("没有这个箱子/效果「%s」。可用 effect: %s；也可直接给箱子中文名、模板名或哈希。"
                % (kind, "、".join(sorted(set(_BURST_EFFECTS.keys())))))
    delay = float(a.get("delay", 0.0) if a.get("delay") is not None else 0.0)
    delay = max(0.0, min(delay, 6.0))
    pos = ""
    if a.get("x") is not None and a.get("y") is not None:
        z = 0 if a.get("z") is None else a.get("z")
        pos = "&x=%s&y=%s&z=%s" % (a["x"], a["y"], z)

    qs = "hash=%s%s%s" % (u["h"], _place_qs(a), pos)
    if delay > 0:
        qs += "&delay=%d" % int(delay * 1000)
    native = None
    try:
        native = api_post("/burst?" + qs)
    except RuntimeError as e:
        if "404" not in str(e):          # 老 DLL 没有 /burst（404）→ 走旧路径；其它错误如实报
            return "引爆失败（%s）：%s" % (u["n"], str(e).strip()[:180])
    if native is not None:
        m = re.search(r"burst=(\d+)\s+obj=([0-9A-Fa-f]+)\s*(.*)", native)
        if not m:
            return "引爆返回异常：%s" % native.strip()[:180]
        ok, obj, why = m.group(1), m.group(2), (m.group(3) or "").strip()
        if ok != "1":
            return "引爆失败（%s）：%s" % (u["n"], why or "未知原因")
        return ("已在落点释放「%s」：箱子 0x%s 由渲染线程在下一帧引爆（效果=死亡开火）"
                % (u["n"], obj))

    # ---- 降级：老 DLL（/burst 返回 404）----
    r = api_post("/summon?hash=%s&count=1%s%s" % (u["h"], _place_qs(a), pos))
    m = re.search(r"objs=\[([0-9A-Fa-f]+)", r)
    if not m:
        return "空投失败：%s" % r.strip()[:200]
    obj = m.group(1)
    if delay > 0:
        time.sleep(delay)
    ok, info = http("/obj?addr=0x%s" % obj)
    j = {}
    if ok:
        try:
            j = json.loads(info)
        except Exception:
            j = {}
    if not ok or not j.get("hash"):
        return "箱子在引爆前已消失（多半被附近单位拾取、效果已提前触发）：%s" % info.strip()[:140]
    if str(j.get("hash", "")).lower() != u["h"].lower():
        # 地址被引擎回收复用（实测：箱子消耗后同地址会分配给新对象）——此时绝不能开火
        return "引爆取消：对象地址已被复用（现为「%s」），未开火。" % j.get("name", "?")
    kr = api_post("/aicall?fn=0x79EA50&this=0x%s&a0=6&a1=1&a2=0&na=3" % obj)
    time.sleep(0.6)
    ok2, info2 = http("/obj?addr=0x%s" % obj)
    gone = (not ok2) or ('"hash"' not in (info2 or ""))
    return ("已在落点释放「%s」：箱子 0x%s %s；引爆调用 %s"
            % (u["n"], obj, "已消耗" if gone else "仍存在（销毁可能有延迟）", kr.strip()[:110]))


_SPAWN_KINDS = {"view": "/view", "vision": "/view", "视野": "/view", "视野中心": "/view",
                "selection": "/selected_pos", "selected": "/selected_pos",
                "选中": "/selected_pos", "选中单位": "/selected_pos",
                "cursor": "/cursor", "mouse": "/cursor", "光标": "/cursor", "鼠标": "/cursor",
                "auto": "/spot", "自动": "/spot"}


def t_spawn_point(a):
    """查一个可用落点。which: view 视野中心（默认）/ selection 选中单位 / cursor 光标 / auto 自动挑。"""
    which = str(a.get("which") or a.get("place") or "view").strip().lower()
    return api_get(_SPAWN_KINDS.get(which, "/view"))


_SEL_ACTIONS = {
    "copy": "/copy", "wnb": "/wnb", "mcv": "/mcv",
    "capture": "/capture", "kill": "/kill", "rank": "/rank",
}


def t_selected_action(a):
    act = str(a.get("action", "")).strip().lower()
    if act not in _SEL_ACTIONS:
        return "action 必须是 %s 之一（都作用于游戏里当前选中的单位）" % "/".join(_SEL_ACTIONS)
    return "%s → %s" % (act, api_post(_SEL_ACTIONS[act]))


def t_set_units(a):
    outs = []
    speed = a.get("speed")
    if speed is not None:
        m = {"fast": 0, "slow": 1, "freeze": 2, "restore": 3}.get(str(speed).lower())
        if m is None:
            outs.append("speed 需为 fast/slow/freeze/restore")
        else:
            outs.append("speed=%s → %s" % (speed, api_post("/speed?mode=%d" % m)))
    health = a.get("health")
    if health is not None:
        m = {"god": 0, "invincible": 0, "hurt": 1, "restore": 2}.get(str(health).lower())
        if m is None:
            outs.append("health 需为 god/hurt/restore")
        else:
            outs.append("health=%s → %s" % (health, api_post("/health?mode=%d" % m)))
    return "\n".join(outs) if outs else "至少给 speed 或 health 一个参数"


_STATUS_KINDS = ("ironcurtain", "invulnerable", "speedup", "slow", "brittle", "damageup")


def t_unit_status(a):
    """给选中单位施加/解除引擎状态，或一键重置（撤掉全部状态 + 速度/血量还原）。"""
    kind = str(a.get("status", "")).strip().lower()
    if kind == "reset" or a.get("reset"):
        return "重置单位状态 → %s" % api_post("/status?kind=reset")
    if kind not in _STATUS_KINDS:
        return "status 必须是 %s 或 reset" % "/".join(_STATUS_KINDS)
    on = a.get("on", True)
    if isinstance(on, str):
        on = on.strip().lower() not in ("0", "off", "false", "no", "解除")
    on = 1 if on else 0
    sec = max(1, min(int(a.get("sec", 60)), 600))
    return "%s(%s) → %s" % (kind, "apply" if on else "remove",
                            api_post("/status?kind=%s&on=%d&sec=%d" % (kind, on, sec)))


# 功能开关：名字 -> (索引, 说明, 取值范围)
FEATURES = {
    "zoom_in": (0x0F, "视野放大", (0, 1)),
    "zoom_out": (0x10, "视野缩小", (0, 1)),
    "power": (8, "电力无限", (0, 1)),
    "scpoint": (9, "科技点无限", (0, 1)),
    "haveallsc": (10, "全科技", (0, 1)),
    "fastbuild": (12, "快速建造", (0, 1)),
    "superpower": (0x0D, "超武就绪", (0, 1)),
    "disablesp": (0x0E, "禁用敌方超武", (0, 1)),
    "map": (0x11, "全图/去迷雾", (0, 1)),
    "unitammo": (0x12, "选中单位弹药满", (0, 1)),
    "danger": (0x13, "危险等级(0不干预/1最高/2最低)", (0, 2)),
    "oremine": (0x14, "恢复矿场(一次性)", (0, 1)),
    "enemybuild": (0x15, "敌方无法建造(0压5000/1压1)", (0, 1)),
    "godmode": (0x16, "玩家无敌", (0, 1)),
    "killmode": (0x17, "秒杀模式", (0, 1)),
}


def t_toggle_feature(a):
    name = str(a.get("feature", "")).strip().lower()
    if name not in FEATURES:
        return "未知开关「%s」。可用：%s" % (name, ", ".join(sorted(FEATURES)))
    idx, desc, _rng = FEATURES[name]
    val = int(a.get("value", a.get("on", 1)))
    if name == "danger":
        val = max(0, min(val, 2))
    else:
        val = 1 if val else 0
    return "%s(%s) = %d → %s" % (name, desc, val, api_post("/toggle?f=%d&on=%d" % (idx, val)))


def t_freebuild(a):
    on = 1 if a.get("on", True) else 0
    return "免费建造 = %d → %s" % (on, api_post("/freebuild?on=%d" % on))


def t_log(a):
    n = int(a.get("n", 40))
    return api_get("/log?n=%d" % max(1, min(n, 200)))


def t_help(_):
    return api_get("/help")


def _with_query(path, query):
    if not query:
        return path
    return path + ("&" if "?" in path else "?") + query


def t_raw(a):
    if not ALLOW_RAW:
        return "raw 已关闭（RA3_MCP_ALLOW_RAW=0）—— 默认是开放的，去掉这个环境变量即恢复。"
    op = str(a.get("op", "")).lower()
    path = str(a.get("path", "") or "")
    query = str(a.get("query", "") or "")
    if op == "get":
        if not path.startswith("/"):
            return "path 需以 / 开头，例如 path=/objects query=limit=5"
        return api_get(_with_query(path, query))
    if op == "post":
        if not path.startswith("/"):
            return "path 需以 / 开头，例如 path=/toggle query=f=12&on=1"
        return api_post(_with_query(path, query))
    if op == "peek":
        return api_get("/peek?addr=%s&type=%s" % (a.get("addr"), a.get("type", "u32")))
    if op == "read":
        return api_get("/read?addr=%s&n=%s" % (a.get("addr"), a.get("n", 64)))
    if op == "poke":
        return api_post("/poke?addr=%s&type=%s&val=%s"
                        % (a.get("addr"), a.get("type", "u32"), a.get("val")))
    if op == "call":
        return api_post("/call?rva=%s&cc=%s&a0=%s&a1=%s&a2=%s"
                        % (a.get("rva"), a.get("cc", "cdecl"), a.get("a0", 0),
                           a.get("a1", 0), a.get("a2", 0)))
    return "op 需为 get/post/peek/read/poke/call"


# ============================ MCP 工具表 ============================
TOOLS = [
    {
        "name": "get_battle_state",
        "description": "读取当前战场态势：我方与各敌方的部队/建筑数量、敌方阵营与兵种构成、"
                       "资金、当前选中单位、扫描统计。这是 AI 的『眼睛』，决策前先调用。",
        "inputSchema": {"type": "object", "properties": {}, "additionalProperties": False},
    },
    {
        "name": "get_enemy_report",
        "description": "只返回敌方（非本地玩家）的明细：每个归属的阵营、部队数、建筑数与兵种构成。",
        "inputSchema": {"type": "object", "properties": {}, "additionalProperties": False},
    },
    {
        "name": "search_units",
        "description": "检索单位目录（1000+ 条）。支持中文名、拼音首字母/全拼、英文模板名、8 位哈希。"
                       "召唤/空投前用它确认真实名字。",
        "inputSchema": {
            "type": "object",
            "properties": {
                "query": {"type": "string", "description": "关键词，如 '磁暴' 或 'cbtk' 或 'CelestialTank'"},
                "limit": {"type": "integer", "description": "返回条数（默认 20，上限 100）"},
                "crates_only": {"type": "boolean", "description": "只搜箱子"},
                "summonable_only": {"type": "boolean", "description": "只搜可召唤的（默认 true）"},
            },
            "required": ["query"],
        },
    },
    {
        "name": "summon_unit",
        "description": "召唤单位到战场。unit 可给中文名、模板名或哈希。"
                       "落点用 place 指定：view=视野中心（玩家此刻在看的位置，推荐）、"
                       "selection=选中单位处、cursor=鼠标处；不给则按修改器的落点设置（默认视野中心）。"
                       "★ 不要自己编 x/y/z —— 凭空写的世界坐标多半在水面上，单位会生成到海里。"
                       "确实要指定点，先用 get_spawn_point 或 list_objects 拿真实坐标。"
                       "owner 可指定归属（0/缺省=你的阵营；>0 = 玩家索引，用于给对手加兵/测试）。",
        "inputSchema": {
            "type": "object",
            "properties": {
                "unit": {"type": "string", "description": "单位名 / 模板名 / 哈希"},
                "count": {"type": "integer", "description": "数量（默认 1，上限 30）"},
                "owner": {"type": "integer",
                          "description": "归属：0/缺省=本地玩家；>0 = 玩家索引（见 get_battle_state 的 enemies[].idx）"},
                "place": {"type": "string", "enum": ["view", "selection", "cursor"],
                          "description": "落点来源：view 视野中心 / selection 选中单位 / cursor 鼠标。缺省=按修改器设置"},
                "x": {"type": "number", "description": "落点世界坐标 X（仅当你已从 get_spawn_point / list_objects 拿到真实坐标时才用）"},
                "y": {"type": "number", "description": "落点世界坐标 Y"},
                "z": {"type": "number", "description": "落点世界坐标 Z（可省）"},
            },
            "required": ["unit"],
        },
    },
    {
        "name": "get_spawn_point",
        "description": "查一个可用的投放落点（世界坐标）。which: view=视野中心（默认，屏幕正中看出去的地面点，"
                       "就是玩家此刻在看的位置）、selection=当前选中单位处、cursor=鼠标处、auto=按设置自动挑。"
                       "返回 water=1 表示该点在水面（别往那儿投陆军/建筑）。"
                       "★ 想知道『该投哪里』就问这个工具；召唤/建造不给落点时也会自动用视野中心。",
        "inputSchema": {
            "type": "object",
            "properties": {
                "which": {"type": "string", "enum": ["view", "selection", "cursor", "auto"],
                          "description": "落点来源（默认 view）"},
            },
        },
    },
    {
        "name": "drop_crate",
        "description": "空投奖励箱。kind 可为 '治疗'/'现金'/'晋升'/'随机' 等中文名或模板名或哈希。"
                       "落点用 place 指定（默认视野中心）。",
        "inputSchema": {
            "type": "object",
            "properties": {
                "kind": {"type": "string", "description": "箱子种类（默认随机奖励箱）"},
                "count": {"type": "integer", "description": "数量（默认 1，上限 30）"},
                "place": {"type": "string", "enum": ["view", "selection", "cursor"],
                          "description": "落点来源（缺省=按修改器设置）"},
            },
            "required": ["kind"],
        },
    },
    {
        "name": "burst_crate",
        "description": "在目标位置空投一个箱子并立即引爆——用箱子的『死亡开火』在落点释放效果"
                       "（冰冻箱=冻结附近单位、炸弹箱=爆炸、超级炸弹/真空弹/粒子炮/雷达干扰等）。"
                       "适合『在指定坐标释放某个效果』，不用等单位去拾取。"
                       "effect 支持 freeze/bomb/superbomb/vacuum/particle/radar/telekinetic/emotion/heal/money 等别名，"
                       "也可给箱子中文名、模板名或哈希。"
                       "★ 效果对敌我无差别（实测会冻到自己人），优先丢在敌方头上。"
                       "★ 属性箱（铁幕/加速/护甲等）的效果走拾取链、不在死亡链上，本工具不适用。",
        "inputSchema": {
            "type": "object",
            "properties": {
                "effect": {"type": "string", "description": "效果别名或箱子名（默认 freeze）"},
                "x": {"type": "number", "description": "落点世界坐标 X（与 y 一起给才生效）"},
                "y": {"type": "number", "description": "落点世界坐标 Y"},
                "z": {"type": "number", "description": "落点世界坐标 Z（可省）"},
                "place": {"type": "string", "enum": ["view", "selection", "cursor"],
                          "description": "不给 x/y 时的落点来源（缺省=修改器设置）"},
                "delay": {"type": "number", "description": "额外等待的秒数（默认 0 = 创建后下一帧就引爆；一般不用改）"},
            },
        },
    },
    {
        "name": "selected_action",
        "description": "对游戏里当前选中的单位执行动作：copy 复制 / wnb 预设增援×8 / mcv 三阵营基地车 / "
                       "capture 收编 / kill 摧毁 / rank 满级。注意：需要先有选中单位。",
        "inputSchema": {
            "type": "object",
            "properties": {
                "action": {"type": "string", "enum": ["copy", "wnb", "mcv", "capture", "kill", "rank"]}
            },
            "required": ["action"],
        },
    },
    {
        "name": "set_units",
        "description": "调整当前选中单位的速度/血量。speed: fast/slow/freeze/restore；health: god/hurt/restore（恢复血量）。",
        "inputSchema": {
            "type": "object",
            "properties": {
                "speed": {"type": "string", "enum": ["fast", "slow", "freeze", "restore"]},
                "health": {"type": "string", "enum": ["god", "hurt", "restore"]},
            },
        },
    },
    {
        "name": "unit_status",
        "description": "给当前选中单位施加/解除引擎状态，或一键重置。status: ironcurtain 铁幕（无敌+外观）/ "
                       "invulnerable 无敌 / speedup 加速 / slow 减速 / "
                       "brittle 易碎（护甲削弱+血量上限压制）/ damageup 输出增强 / reset 重置全部（撤所有状态，"
                       "移动速度与血量一并还原）。需要先有选中单位（见 get_battle_state 的 selected）。",
        "inputSchema": {
            "type": "object",
            "properties": {
                "status": {"type": "string",
                           "enum": ["ironcurtain", "invulnerable", "speedup", "slow",
                                    "brittle", "damageup", "reset"]},
                "on": {"type": "boolean", "description": "true=施加（默认）/ false=解除（对 reset 无效）"},
                "sec": {"type": "integer", "description": "时长（秒，默认 60，上限 600）"},
            },
            "required": ["status"],
        },
    },
    {
        "name": "toggle_feature",
        "description": "切换修改器功能开关。可用：power 电力无限 / scpoint 科技点 / haveallsc 全科技 / "
                       "fastbuild 快速建造 / superpower 超武就绪 / disablesp 禁敌超武 / map 去迷雾 / "
                       "unitammo 弹药满 / godmode 玩家无敌 / killmode 秒杀 / enemybuild 敌不可建 / "
                       "danger 危险等级(0/1/2) / zoom_in / zoom_out / oremine 恢复矿场。",
        "inputSchema": {
            "type": "object",
            "properties": {
                "feature": {"type": "string", "description": "开关名"},
                "value": {"type": "integer", "description": "取值（多数 0/1；danger 为 0/1/2）"},
            },
            "required": ["feature"],
        },
    },
    {
        "name": "set_freebuild",
        "description": "免费建造开关（建造不花钱）。",
        "inputSchema": {
            "type": "object",
            "properties": {"on": {"type": "boolean", "description": "true=开"}},
            "required": ["on"],
        },
    },
    {
        "name": "get_log",
        "description": "读取修改器的输出记录（最近事件，排查失败原因用）。",
        "inputSchema": {
            "type": "object",
            "properties": {"n": {"type": "integer", "description": "行数（默认 40）"}},
        },
    },
    {
        "name": "get_status",
        "description": "查看接口状态：监听地址、已服务调用次数。（接口恒开放，无需任何开关。）",
        "inputSchema": {"type": "object", "properties": {}, "additionalProperties": False},
    },
    {
        "name": "list_objects",
        "description": "【感知】列出战场上真实存在的对象（带内存地址）。每个条目含：thing（对象地址，指挥/攻击用它）、"
                       "ai（AI 指针）、坐标 x/y/z、当前/上限血量 hp/hpMax、归属 owner（玩家索引）、阵营、类型。"
                       "这是 AI 的『眼睛』——想知道『我的航母在哪』『敌方基地还有多少血』就调它，"
                       "然后把 thing 地址交给指挥类工具。无需人手点选。",
        "inputSchema": {
            "type": "object",
            "properties": {
                "query": {"type": "string", "description": "名字子串（如 '航母'）或哈希十六进制前缀；缺省=不过滤"},
                "owner": {"type": "integer", "description": "玩家索引过滤（见 get_battle_state 的 myUnits/enemies）；缺省=全部"},
                "cls": {"type": "string", "description": "类型过滤：其他/步兵/载具/空军/海军/建筑/箱子（或 0-6）"},
                "cat": {"type": "string", "description": "大类过滤：unit/struct/other（或 单位/建筑/其他）"},
                "limit": {"type": "integer", "description": "返回条数（默认 40，上限 100）"},
                "fresh": {"type": "boolean", "description": "强制重新全图扫描（默认用 3 秒缓存）"},
                "include_unknown": {"type": "boolean",
                                    "description": "true=连场景物/模块子对象/空槽一起返回（默认 false 只给真单位与建筑）"},
            },
            "additionalProperties": False,
        },
    },
    {
        "name": "object_detail",
        "description": "【感知】查某个对象的完整状态（按 Thing 地址，地址来自 list_objects）。"
                       "返回 Thing/AI/血量体地址、精确坐标、当前与上限血量、归属。",
        "inputSchema": {
            "type": "object",
            "properties": {
                "addr": {"type": "string", "description": "Thing 地址，如 '0x05803640'"},
            },
            "required": ["addr"],
        },
    },
    {
        "name": "command_unit",
        "description": "【指挥】命令一个单位行动（直调其 AI）。attack：攻击目标；move：走向世界坐标；idle：待机。"
                       "先 list_objects 拿到『单位』与『目标』的 thing 地址，攻击后可用 list_objects 看目标血量是否下降。"
                       "本机 127.0.0.1 单机接口，无需任何开关；联机禁用。",
        "inputSchema": {
            "type": "object",
            "properties": {
                "unit": {"type": "string", "description": "被指挥单位的 Thing 地址（list_objects 的 thing 字段）"},
                "act": {"type": "string", "enum": ["attack", "move", "idle"],
                        "description": "动作：attack 攻击 / move 移动 / idle 待机"},
                "target": {"type": "string", "description": "attack 时的目标 Thing 地址（list_objects 的 thing 字段）"},
                "x": {"type": "number", "description": "move 的目标世界坐标 X"},
                "y": {"type": "number", "description": "move 的目标世界坐标 Y"},
                "z": {"type": "number", "description": "move 的目标世界坐标 Z（可省，默认 0）"},
            },
            "required": ["unit", "act"],
        },
    },
    {
        "name": "build_structure",
        "description": "【建造】把一座建筑直接造出来（建筑也有对象代码，等价于召唤）。"
                       "注意：属『直接建造』——建筑立刻完整出现，不走游戏建造队列、不检查科技前置与资源。"
                       "建筑名用 search_units(cls='建筑') 查。落点用 place 指定（默认视野中心），"
                       "★ 不要自己编 x/y。",
        "inputSchema": {
            "type": "object",
            "properties": {
                "unit": {"type": "string", "description": "建筑名 / 模板名 / 哈希"},
                "place": {"type": "string", "enum": ["view", "selection", "cursor"],
                          "description": "落点来源（缺省=按修改器设置）"},
                "x": {"type": "number", "description": "落点世界坐标 X（可选；仅在已有真实坐标时用）"},
                "y": {"type": "number", "description": "落点世界坐标 Y（可选）"},
                "z": {"type": "number", "description": "落点世界坐标 Z（可选）"},
                "owner": {"type": "integer", "description": "归属：0/缺省=本地玩家；>0=玩家索引"},
            },
            "required": ["unit"],
        },
    },
]

HANDLERS = {
    "list_objects": t_list_objects,
    "object_detail": t_object_detail,
    "command_unit": t_command_unit,
    "build_structure": t_build_structure,
    "get_battle_state": t_game_state,
    "get_enemy_report": t_enemy_report,
    "search_units": t_search_units,
    "summon_unit": t_summon,
    "drop_crate": t_drop_crate,
    "burst_crate": t_burst_crate,
    "get_spawn_point": t_spawn_point,
    "selected_action": t_selected_action,
    "set_units": t_set_units,
    "unit_status": t_unit_status,
    "toggle_feature": t_toggle_feature,
    "set_freebuild": t_freebuild,
    "get_log": t_log,
    "get_status": t_status,
}

if ALLOW_RAW:
    TOOLS.append({
        "name": "raw",
        "description": ("通用透传：op=get/post 可调调试接口的任意路由（等同直连 HTTP，含 /grant /upgrade /power /protocol "
                        "/buildable /aicall /action /lua /diag /snapshot /read /resolve /poke 等）；"
                        "op=peek/read/poke/call 直接读写内存、按 RVA 调函数。"),
        "inputSchema": {
            "type": "object",
            "properties": {
                "op": {"type": "string", "enum": ["get", "post", "peek", "read", "poke", "call"]},
                "path": {"type": "string", "description": "以 / 开头，如 /objects 或 /grant?power=SpecialPower_X"},
                "query": {"type": "string", "description": "追加查询串，如 f=12&on=1"},
                "addr": {"type": "string"}, "type": {"type": "string"},
                "val": {"type": "string"}, "n": {"type": "integer"},
                "rva": {"type": "string"}, "cc": {"type": "string"},
                "a0": {"type": "integer"}, "a1": {"type": "integer"}, "a2": {"type": "integer"},
            },
            "required": ["op"],
        },
    })
    HANDLERS["raw"] = t_raw


# ============================ JSON-RPC / MCP ============================
def send(obj):
    sys.stdout.write(json.dumps(obj, ensure_ascii=False) + "\n")
    sys.stdout.flush()


def reply(mid, result):
    send({"jsonrpc": "2.0", "id": mid, "result": result})


def rpc_error(mid, code, message):
    send({"jsonrpc": "2.0", "id": mid, "error": {"code": code, "message": message}})


def handle(msg):
    if not isinstance(msg, dict):
        return
    mid = msg.get("id")
    method = msg.get("method")
    params = msg.get("params") or {}

    if method == "initialize":
        reply(mid, {
            "protocolVersion": params.get("protocolVersion") or DEFAULT_PROTOCOL,
            "capabilities": {"tools": {"listChanged": False}},
            "serverInfo": {"name": SERVER_NAME, "version": SERVER_VERSION},
            "instructions": ("红警3 日冕修改器的脑机接口。决策前先 get_battle_state 读局势；"
                             "本机 127.0.0.1 单机接口，所有工具开箱即用、无需任何开关；联机禁用。"
                             "接口说明见 docs/AI接入指南.md。"),
        })
        return
    if method in ("notifications/initialized", "initialized", "notifications/cancelled"):
        return
    if method == "ping":
        reply(mid, {})
        return
    if method == "tools/list":
        reply(mid, {"tools": TOOLS})
        return
    if method == "resources/list":
        reply(mid, {"resources": []})
        return
    if method == "prompts/list":
        reply(mid, {"prompts": []})
        return
    if method == "tools/call":
        name = params.get("name")
        args = params.get("arguments") or {}
        fn = HANDLERS.get(name)
        if not fn:
            rpc_error(mid, -32602, "未知工具 %s" % name)
            return
        try:
            text = fn(args)
            reply(mid, {"content": [{"type": "text", "text": str(text)}], "isError": False})
        except Exception as e:                       # 工具内失败 → 作为内容返回，不中断会话
            reply(mid, {"content": [{"type": "text", "text": "执行失败：%s" % e}], "isError": True})
        return
    if mid is not None:
        rpc_error(mid, -32601, "未实现的方法 %s" % method)


def read_message(stream):
    """读一条消息：兼容 MCP 的两种 stdio 分帧。
       · 换行分隔 JSON（MCP 规范默认）—— 行首是 '{'
       · LSP 式 Content-Length: N \\r\\n\\r\\n<body>
       注意：JSON 里本身含冒号，必须先判行首 '{'，不能按 ':' 判 header。"""
    while True:
        line = stream.readline()
        if not line:
            return None
        s = line.strip()
        if not s:
            continue
        if s.startswith("{"):
            try:
                return json.loads(s)
            except Exception:
                continue
        if ":" in s:
            k, v = s.split(":", 1)
            if k.strip().lower() == "content-length":
                try:
                    n = int(v.strip())
                except ValueError:
                    continue
                body = stream.read(n)
                try:
                    return json.loads(body)
                except Exception:
                    return None
            continue


def selftest():
    """离线自检：不依赖游戏，验证目录与工具表。"""
    print("=== Ra3-Copilot MCP 自检 ===")
    print("接口       : %s" % API)
    print("目录       : %d 单位 / %d 箱子（%s）"
          % (len(CAT.units), len(CAT.crates), CATALOG_PATH))
    print("raw 工具   : %s" % ("已注册（危险）" if ALLOW_RAW else "未注册（默认安全）"))
    print("工具 %d 个 : %s" % (len(TOOLS), ", ".join(t["name"] for t in TOOLS)))
    for q in ("磁爆", "cbtk", "CelestialTank"):
        hits = CAT.search(q, 3)
        print("  检索 %-14s -> %s" % (q, "；".join("%s(%s)" % (u["n"], u["h"]) for u in hits) or "无"))
    ok, txt = http("/ping")
    print("游戏接口   : %s" % ("pong ✅" if txt.strip() == "pong" else "不可用（%s）" % txt[:80]))
    ok, txt = http("/ai/status")
    print("脑机状态   : %s" % (txt.strip()[:160] if ok else "不可用（%s）" % txt[:80]))
    ok, txt = http("/snapshot")
    print("战场快照   : %s" % (txt.strip()[:400] if ok else "不可用（%s）" % txt[:80]))


def main():
    if "--selftest" in sys.argv:
        selftest()
        return
    log("启动：API=%s  raw=%s  目录=%s" % (API, ALLOW_RAW, os.path.basename(CATALOG_PATH)))
    stream = sys.stdin
    while True:
        try:
            msg = read_message(stream)
        except Exception as e:
            log("读取出错：%s" % e)
            break
        if msg is None:
            break
        try:
            handle(msg)
        except Exception as e:
            log("处理出错：%s" % e)


if __name__ == "__main__":
    main()
