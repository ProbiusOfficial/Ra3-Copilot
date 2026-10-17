#!/usr/bin/env python3
"""modifier_apply.py — 对单位施加/解除 modifier（AttributeModifier）

目标两种指定：
  --obj 0x..      直接给一个目标 Thing 地址
  --selected      对「游戏里当前选中的单位」（全体）操作 —— 外部只读解析选中链，免改 DLL

效果三种指定：
  --effect <名>   常用效果预设（--list-effects 查看）
  --name <名>     任意 modifier 名（sage_hash 现算）
  --hash 0x..     直接给名字哈希

链路（全部走游戏内 HTTP 接口，不需要额外接口）：
  1. POST /call?rva=0xA6BE0&cc=cdecl&a0=0xC5E07887&a1=<名字哈希>&a2=0
     → modifier 库查表（bool cdecl: container, nameHash, out）
  2. POST /aicall?fn=0x7181F0&this=<Thing>&a0=<modifier>&a1=<时长>&na=2   → 施加
     POST /aicall?fn=0x718300&this=<Thing>&a0=<modifier>&na=1             → 解除

时长单位 = 帧（15/秒）。默认 900（≈60 秒）；-1 实测约 12 秒后过期，不建议依赖。
参数顺序依据：引擎内 34 处调用点全部是 `push <时长> ; push <modifier> ; mov ecx, <宿主对象>`。

选中链解析（--selected，外部只读，与修改器 actions.cpp 的 SelIter 同源）：
  全局 VA 0xCDB73C → mgr；mgr+0x50 = 链表头，mgr+0x5C = 数量；
  节点：+8 = 对象（通常 Drawable），+0 = next。
  归一为 Thing：先试 [obj+0x138]（Drawable→宿主 Thing），
  否则用 Thing 判据 [obj+0x33C]（召唤物自身即 Thing）。

用法：
  python modifier_apply.py --list-effects
  python modifier_apply.py --selected --effect ironcurtain            # 全部选中单位
  python modifier_apply.py --selected --effect ironcurtain --remove   # 解除
  python modifier_apply.py --selected --name AttributeModifier_IronCurtain --dur 900
  python modifier_apply.py --selected --dry                           # 只看：链上解析出谁 + 查表结果
  python modifier_apply.py --name AttributeModifier_IronCurtain --obj 0x072A36A0
  python modifier_apply.py --list [--owner N] [--q 名字]               # 列对象（挑目标用）

注意：modifier 对象是**引擎拥有的资源**，不要释放、不要写它的内存；本工具只做"查表 + 施加/解除"。
"""
import argparse
import ctypes
import json
import os
import re
import subprocess
import sys
import urllib.request

RVA_MODIFIER_FIND = 0xA6BE0      # modifier 库查表器（cdecl: container, nameHash, out）
VA_MODIFIER_APPLY = 0x7181F0     # 施加（thiscall: this, modifier, duration；vtable+0x1D0；ret 8）
VA_MODIFIER_REMOVE = 0x718300    # 解除（thiscall: this, modifier；vtable+0x1D8；ret 4）
MODIFIER_STORE = 0xC5E07887      # 全局 modifier 库容器哈希
DUR_DEFAULT = 900                # 时长默认值（帧；900 ≈ 60 秒）
GAME_IMAGE = 'ra3_1.12.game'

# 选中链（RVA 口径；VA = RVA + 0x400000）——与 actions.cpp 注释一致
SEL_MGR_GLOB_VA = 0xCDB73C
OFF_SEL_HEAD = 0x50
OFF_SEL_COUNT = 0x5C

# 效果预设：名字 → (modifier 名, 默认时长(帧), 说明)
#   说明口径见 research/protocol-skills/findings/modifier-施加通道.md §五/§八（实测定案）
EFFECT_PRESETS = {
    'ironcurtain':  ('AttributeModifier_IronCurtain',         900, '铁幕：无敌 + 铁幕外观（实测可见）'),
    'freeze':       ('AttributeModifer_CryoBeam_10',          900, '冷冻10档：冰涂装+定身+技能锁+周围禁建（无易碎）'),
    'frozenmodel':  ('AttributeMod_MAP_ObjectsFrozen',        900, '实心冰块模型（纯视觉；真冻单位同款冰覆盖资源）'),
    'icemodel':     ('AttributeMod_MAP_ObjectsIronCurtain',   900, '铁幕外观模型（纯视觉）'),
    'invulnerable': ('AttributeModifier_MAP_INVULNERABLE_Up', 900, '无敌（MAP 系列）'),
    'slow':         ('AttributeModifier_MAP_SPEED_Down',      900, '减速'),
    'speedup':      ('AttributeModifier_MAP_SPEED_Up',        900, '加速'),
    'armor_up':     ('AttributeModifier_MAP_ARMOR_Up',        900, '护甲削弱（实测承伤 ×10.9，疑似触底）'),
    'armor_down':   ('AttributeModifier_MAP_ARMOR_Down',      900, '护甲加强'),
    'health_down':  ('AttributeModifier_MAP_HEALTH_MULT_Down', 900, '血量上限 ×0.9/层（线性可叠）'),
    'health_up':    ('AttributeModifier_MAP_HEALTH_MULT_Up',  900, '血量上限 +10%/层'),
    'damage_up':    ('AttributeModifier_MAP_DAMAGE_MULT_Up',  900, '自身输出伤害提升'),
    'damage_down':  ('AttributeModifier_MAP_DAMAGE_MULT_Down', 900, '自身输出伤害降低'),
}


def sage_hash_of(name):
    """名字 → SAGE 哈希（小写入参）。复用 unit-codes 的复算脚本，避免两份实现。"""
    here = os.path.dirname(os.path.abspath(__file__))
    sys.path.insert(0, os.path.join(here, '..', '..', 'unit-codes', 'tools'))
    from sage_hash import sage_hash          # noqa: E402
    return sage_hash(name.lower())


class Api:
    def __init__(self, port):
        self.base = f'http://127.0.0.1:{port}'

    def post(self, path, **params):
        q = '&'.join(f'{k}={v}' for k, v in params.items() if v is not None)
        url = f'{self.base}{path}' + (f'?{q}' if q else '')
        with urllib.request.urlopen(url, data=b'', timeout=10) as r:
            return r.read().decode('utf-8', 'replace').strip()

    def get(self, path, **params):
        q = '&'.join(f'{k}={v}' for k, v in params.items() if v is not None)
        url = f'{self.base}{path}' + (f'?{q}' if q else '')
        with urllib.request.urlopen(url, timeout=10) as r:
            return r.read().decode('utf-8', 'replace').strip()


# ---------- 选中链（外部只读） ----------

def find_game_pid():
    out = subprocess.run(
        ['tasklist', '/fi', f'imagename eq {GAME_IMAGE}', '/nh', '/fo', 'csv'],
        capture_output=True, text=True, errors='replace')
    m = re.findall(rf'"{re.escape(GAME_IMAGE)}","(\d+)"', out.stdout)
    return int(m[0]) if m else None


class Mem:
    def __init__(self, pid):
        self.k32 = ctypes.WinDLL('kernel32', use_last_error=True)
        self.h = self.k32.OpenProcess(0x0410, False, pid)   # QUERY_INFORMATION | VM_READ
        if not self.h:
            raise RuntimeError(f'OpenProcess 失败（pid={pid}，err={ctypes.get_last_error()}）')

    def read(self, addr, n):
        buf = ctypes.create_string_buffer(n)
        got = ctypes.c_size_t(0)
        ok = self.k32.ReadProcessMemory(self.h, ctypes.c_void_p(addr), buf, n,
                                        ctypes.byref(got))
        return buf.raw[:got.value] if ok and got.value >= n else None

    def u32(self, addr):
        d = self.read(addr, 4)
        return int.from_bytes(d, 'little') if d else None

    def close(self):
        self.k32.CloseHandle(self.h)


def _normalize_thing(mem, obj):
    """与 actions.cpp BodyOf 同口径：Drawable → [obj+0x138]；召唤物自身即 Thing（[+0x33C] 判据）。"""
    b = mem.u32(obj + 0x138)
    if b and mem.read(b, 0x44):
        return b
    hp = mem.u32(obj + 0x33C)
    if hp and mem.read(hp, 0x14):
        return obj
    return None


def walk_selected():
    """返回 [(obj, thing), ...]。链为空时返回 []。"""
    pid = find_game_pid()
    if not pid:
        print(f'[x] 找不到进程 {GAME_IMAGE}（游戏没开？）', file=sys.stderr)
        return None
    mem = Mem(pid)
    try:
        mgr = mem.u32(SEL_MGR_GLOB_VA)
        if not mgr:
            print('[x] 选中管理器未就绪（还没进对局？）', file=sys.stderr)
            return None
        count = mem.u32(mgr + OFF_SEL_COUNT) or 0
        node = mem.u32(mgr + OFF_SEL_HEAD)
        if count <= 0 or not node:
            return []
        out = []
        head = node
        for _ in range(min(count, 256)):
            obj = mem.u32(node + 8)
            nxt = mem.u32(node)
            if not obj:
                break
            thing = _normalize_thing(mem, obj)
            if thing:
                out.append((obj, thing))
            node = nxt
            if node in (0, head):
                break
        return out
    finally:
        mem.close()


def cmd_list_effects():
    print(f'{"预设":<14} {"modifier 名":<38} {"时长":>6}  说明')
    for k, (name, dur, desc) in EFFECT_PRESETS.items():
        print(f'{k:<14} {name:<38} {dur:>6}  {desc}')


def parse_ret(receipt):
    """从 'call rva=.. ret=XXXXXXXX' 之类的回执里取 16 进制返回值。"""
    for tok in receipt.replace('=', ' ').split():
        if len(tok) == 8:
            try:
                return int(tok, 16)
            except ValueError:
                continue
    return None


def cmd_list(api, owner, q):
    raw = api.get('/objects', limit=100, owner=owner, q=q)
    data = json.loads(raw)
    print(f'共 {data["count"]} 个对象：')
    print(f'{"thing":>12} {"owner":>5} {"local":>5} {"side":<8} {"cls":<10} {"name":<22} hp')
    for o in data['objects']:
        print(f'{o["thing"]:>12} {o["owner"]:>5} {o["local"]:>5} {o["side"]:<8} '
              f'{o["cls"]:<10} {o["name"][:20]:<22} {o["hp"]:.0f}/{o["hpMax"]:.0f}')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--port', type=int, default=17832)
    ap.add_argument('--list', action='store_true', help='只列对象（挑目标用）')
    ap.add_argument('--list-effects', action='store_true', help='列出效果预设')
    ap.add_argument('--owner', type=int, default=None)
    ap.add_argument('--q', default=None)
    ap.add_argument('--effect', default=None, help='效果预设名（见 --list-effects）')
    ap.add_argument('--name', default=None, help='modifier 名（如 AttributeModifier_IronCurtain）')
    ap.add_argument('--hash', default=None, help='直接给名字哈希')
    ap.add_argument('--obj', default=None, help='目标 Thing 地址（单个）')
    ap.add_argument('--selected', action='store_true', help='对游戏里当前选中单位（全体）操作')
    ap.add_argument('--dur', type=int, default=None, help=f'时长（帧，15/秒）；默认按预设或 {DUR_DEFAULT}')
    ap.add_argument('--dry', action='store_true', help='只查表/列目标，不施加')
    ap.add_argument('--remove', action='store_true', help='解除该 modifier（引擎 0x718300）')
    a = ap.parse_args()

    if a.list_effects:
        cmd_list_effects()
        return 0

    api = Api(a.port)

    if a.list:
        cmd_list(api, a.owner, a.q)
        return 0

    # ---- 效果/名字解析 ----
    dur = a.dur
    if a.effect:
        if a.effect not in EFFECT_PRESETS:
            print(f'[x] 未知预设 {a.effect}；可用：{", ".join(EFFECT_PRESETS)}', file=sys.stderr)
            return 2
        preset_name, preset_dur, _ = EFFECT_PRESETS[a.effect]
        if a.name or a.hash:
            print('[x] --effect 与 --name/--hash 互斥', file=sys.stderr)
            return 2
        a.name = preset_name
        if dur is None:
            dur = preset_dur
    if dur is None:
        dur = DUR_DEFAULT

    if not a.name and not a.hash:
        print('[x] 需要 --effect / --name / --hash 之一（目标见 --list 或 --selected）', file=sys.stderr)
        return 2
    if a.selected and a.obj:
        print('[x] --selected 与 --obj 互斥', file=sys.stderr)
        return 2
    if not a.selected and not a.obj:
        print('[x] 需要 --selected 或 --obj 指定目标', file=sys.stderr)
        return 2

    name_hash = int(a.hash, 0) if a.hash else sage_hash_of(a.name)
    label = a.name or a.hash
    print(f'[*] modifier {label}  哈希=0x{name_hash:08X}')

    # ---- 目标解析 ----
    targets = []   # [(label, thing_addr_hex_str)]
    if a.selected:
        pairs = walk_selected()
        if pairs is None:
            return 3
        if not pairs:
            print('[x] 游戏里当前没有选中单位（先在游戏里选中，再执行）', file=sys.stderr)
            return 3
        print(f'[*] 当前选中 {len(pairs)} 个单位：')
        for i, (obj, thing) in enumerate(pairs):
            print(f'      [{i:2d}] obj=0x{obj:08X} → thing=0x{thing:08X}')
        targets = [(f'sel[{i}] 0x{thing:08X}', hex(thing)) for i, (_o, thing) in enumerate(pairs)]
    else:
        targets = [(a.obj, a.obj)]

    # ---- 查表 ----
    r1 = api.post('/call', rva=hex(RVA_MODIFIER_FIND), cc='cdecl',
                  a0=hex(MODIFIER_STORE), a1=hex(name_hash), a2='0')
    print(f'[1] 查表 {r1}')
    mod = parse_ret(r1)
    if not mod:
        print('[x] 查表未命中 —— 该名字在本局数据里不存在，已中止（不施加）', file=sys.stderr)
        return 3
    print(f'    modifier 对象 = 0x{mod:08X}')

    if a.dry:
        print('[=] --dry：到此为止，未施加/解除')
        return 0

    # ---- 逐目标施加/解除 ----
    ok_n = 0
    for tlabel, tobj in targets:
        if a.remove:
            r2 = api.post('/aicall', fn=hex(VA_MODIFIER_REMOVE), **{
                'this': tobj, 'a0': hex(mod), 'na': 1})
            ret = parse_ret(r2)
            good = bool(ret)
            tag = '已解除' if good else '返回 0（没有持有/宿主不对）'
        else:
            r2 = api.post('/aicall', fn=hex(VA_MODIFIER_APPLY), **{
                'this': tobj, 'a0': hex(mod), 'a1': dur, 'na': 2})
            good = ('ret=00000001' in r2) or (parse_ret(r2) == 1)
            tag = '成功' if good else '返回 0（宿主没有处理器，或参数不符；可换 this=Body 再试）'
        ok_n += 1 if good else 0
        print(f'  [{tlabel}] {tag}   回执: {r2}')

    verb = '解除' if a.remove else '施加'
    print(f'[+] {verb}完成：{ok_n}/{len(targets)}'
          + ('' if a.remove else f' ｜ 时长 {dur} 帧'))
    return 0 if ok_n > 0 else 4


if __name__ == '__main__':
    sys.exit(main())
