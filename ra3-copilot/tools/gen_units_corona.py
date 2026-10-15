#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""gen_units_corona.py - 由 unit-codes 的权威表生成 src/units_corona.h

数据源
  <ws>/unit-codes/data/callable_final.tsv        ← 首选：逐条实测「能召唤」的清单（已排除中立）
  <ws>/unit-codes/data/unit_codes_trainer.tsv    ← 退路：未筛选的 718 条原始表
  src/units_corona.h 里现有的 kCrates[]          ← 54 条箱子（哈希沿用，显示名换成中文）
产出
  src/units_corona.h （UTF-8 无 BOM；编译器带 /utf-8）

字段：阵营 / 中文名 / 对象模板名 / **拼音** / 对象类型哈希 / 是否基准形态
  拼音用 pypinyin 预计算并**写死在头文件里**（运行时零依赖）——
  UI 输入 "jdc" 能搜到「基地车」，输入 "jidiche" 也行。
"""
import csv
import io
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)                                  # .../RA3/ra3-copilot
WS = os.path.dirname(ROOT)                                    # .../RA3
D = os.path.join(WS, 'unit-codes', 'data')
FINAL_TSV = os.path.join(D, 'callable_final.tsv')
TSV = os.path.join(D, 'unit_codes_trainer.tsv')
OUT = os.path.join(ROOT, 'src', 'units_corona.h')

FACTION_ORDER = ['神州', '盟军', '苏联', '帝国', '未分类']

# ---------------- 拼音（预计算，运行时零依赖） ----------------
try:
    from pypinyin import lazy_pinyin, Style

    def _alnum(parts):
        # 丢掉 · ↑ ↓ 这类非字母数字符号，避免污染拼音串（"现金箱·多人" → "xjxdr"）
        return ''.join(p for p in parts if p.isalnum())

    def py_of(s):
        if not s or not any('\u4e00' <= c <= '\u9fff' for c in s):
            return ''
        first = _alnum(lazy_pinyin(s, style=Style.FIRST_LETTER))
        full = _alnum(lazy_pinyin(s))
        return ('%s %s' % (first, full)).strip()
except Exception:
    print('[!] 没装 pypinyin → 拼音列留空（UI 拼音搜索会失效）')
    print('    安装：python -m pip install pypinyin -i https://pypi.org/simple --proxy http://127.0.0.1:7890')

    def py_of(s):
        return ''


# ---------------- 中文名缺失（只有英文名）的条目：按功能译名 + 标 [非官方] ----------------
# 日冕的字符串表我们只拿到英文包，这些建筑没有官方中文名；以下是按功能意译，
# UI 上会带 [非官方] 后缀以便和官方译名区分。
UNIT_CN = {
    'CelestialAirfield': '空军停机坪',
    'CelestialBattery': '蓄能鼎',
    'CelestialConstructionYard': '建造厂',
    'CelestialEnergyGatlingTower': '迅雷机炮塔',
    'CelestialEnergyRailgunBase': '天弧电磁炮台',
    'CelestialEngineerDrone': '维修天灯',
    'CelestialLaserTower': '空焰激光塔',
    'CelestialMachineArm': '机械臂',
    'CelestialOutPost': '中继引导站',
    'CelestialPowerPlant': '聚变亭',
    'CelestialPowerPlantAdvanced': '分流尖塔',
    'CelestialRefinery': '矿石熔炉',
    'CelestialSuperWeapon': '日晷阵列',
    'CelestialWallPiece': '铁壁墙段',
    'AlliedAegisLargeDefenseBase': '宙斯盾大型防御基地',
    'AlliedAirPad': '盟军停机坪',
    'AlliedHeavyArtilleryDefenseBase': '施耐德重炮台',
    'AlliedLowTechStructure': '情报中心',
    'SovietHighTechStructure': '战斗指挥堡垒',
    'JapanHighTechStructure': '帝国要员堡垒',
}

UNOFFICIAL = ' [非官方]'

# ---------------- 从游戏内存扒下来的官方中文名（tools/lookup_names_mem.py 生成）----------------
# 日冕新对象的中文名只在**压缩的字符串表**里，磁盘拿不到；这份是在游戏运行时从内存里
# 按「NAME:模板名 → 值」提取的，优先级高于 data_mod.str（后者对部分新对象只有英文名）。
MEM_CN = {}
_mem_path = os.path.join(D, 'cn_names_mem.json')
if os.path.exists(_mem_path):
    try:
        MEM_CN = json.load(io.open(_mem_path, encoding='utf-8'))
    except Exception as e:
        print('[!] cn_names_mem.json 读取失败: %s' % e)

# ---------------- 官方单位图鉴（cor-games.com/units 的数据，隔壁会话抓的）----------------
# display_name = 官方中文名（最权威）；kind_of = SAGE 原生类型标志位（SELECTABLE/VEHICLE/…）
ODB = None
try:
    sys.path.insert(0, os.path.join(WS, 'unit-codes', 'tools'))
    import official_db as _odb
    ODB = _odb
    print('[*] 官方图鉴已载入：%d 条' % len(_odb.load()))
except Exception as e:
    print('[!] 官方图鉴不可用（%s）→ 名字退回内存扒的，类型退回命名启发式' % e)

# kind_of → 显示类型。顺序即优先级：军舰 / 飞机**同时**带 VEHICLE，子类必须先判，
# 否则航母又会被判成"载具"。
OFFICIAL_KIND = [('INFANTRY', '步兵'), ('SHIP', '海军'), ('AIRCRAFT', '空军'),
                 ('VEHICLE', '载具'), ('STRUCTURE', '建筑'), ('CRATE', '箱子')]

# 探宝在游戏里逐字核对过的（内存里那份对个别对象取到了类型标签，这里手工纠正）
CN_OVERRIDE = {
    'AlliedAegisLargeDefenseBase': '埃癸斯护盾发生器',
}


def official_name(tid):
    """官方中文名。优先级：官方图鉴 display_name（最权威）→ 内存扒的 → 手工钉的。
    顺手把误取到的 DESC 句子 / 类型描述剔掉。"""
    v = ''
    if ODB:
        r = ODB.record(tid)
        if r:
            v = (r.get('display_name') or '').strip()
    if not v:
        v = (CN_OVERRIDE.get(tid) or MEM_CN.get(tid) or '').strip()
    if not v:
        return None
    if v.endswith('。') or '/' in v or len(v) > 16:
        return None
    return v


def has_cjk(s):
    return any('\u4e00' <= c <= '\u9fff' for c in (s or ''))


# ---------------- 对象类型推断（离线启发式） ----------------
# SAGE 的**权威**分类是对象模板里的 KindOf 位掩码（需要运行时读内存）；
# 这里先用命名规则推断 —— 日冕/EA 的命名相当规范，取「名字里最后出现的兵种词」，
# 这样 CelestialAntiInfantryVehicle（反步兵**载具**）会正确落到「载具」而不是「步兵」。
# 建筑：先于兵种词判断（否则 AlliedAirPad 会被 "Air" 抓成空军）
BUILD_WORDS = ('Airfield', 'AirPad', 'Barracks', 'Refinery', 'PowerPlant', 'ConstructionYard',
               'WallPiece', 'Tower', 'Turret', 'SentryGun', 'Structure', 'Lab', 'Port',
               'Fortress', 'Factory', 'Yard', 'Bunker', 'OutPost', 'SuperWeapon', 'Statue',
               'CoreObject', 'Defense', 'Defence', 'Perimeter', 'Facility', 'HQ')
# 兵种词：取「名字里最后出现的那个」（CelestialAntiInfantryVehicle → 载具）
RANGED_WORDS = [
    ('Infantry', '步兵'), ('Legionnaire', '步兵'), ('Engineer', '步兵'),
    ('Commando', '步兵'), ('Desolator', '步兵'), ('Warrior', '步兵'),
    ('Vehicle', '载具'), ('Tank', '载具'), ('Truck', '载具'), ('MCV', '载具'),
    ('Artillery', '载具'), ('IFV', '载具'), ('MBT', '载具'), ('FAV', '载具'),
    ('Miner', '载具'), ('Cycle', '载具'), ('Surveyor', '载具'),
    ('Mech', '载具'), ('Mecha', '载具'),
    ('Aircraft', '空军'), ('Fighter', '空军'), ('Bomber', '空军'), ('Drone', '空军'),
    ('Helicopter', '空军'), ('Balloon', '空军'), ('Mig', '空军'),
    ('Ship', '海军'), ('Submarine', '海军'), ('Naval', '海军'), ('Boat', '海军'),
    ('Vessel', '海军'), ('LandingCraft', '海军'), ('Cruiser', '海军'), ('Destroyer', '海军'),
    ('Carrier', '海军'),   # 航空母舰 —— 必须排在 Aircraft 之后仍能胜出（取"最后出现的兵种词"），
                           # 否则 AlliedGaintAirCraft**Carrier** 会被 "AirCraft" 抓成空军
    ('Crate', '箱子'),
]
# 中文名兜底（英文词都没命中时用；顺序 = 从具体到宽泛，先判「飞艇」再判「艇」）
CN_KINDS = [
    (('飞艇', '轰炸机', '战斗机', '截击机', '无人机', '攻击机', '战机', '飞机',
      '直升机', '哨机', '炮艇机', '特攻机'), '空军'),
    (('航空母舰', '驱逐舰', '巡洋舰', '登陆舰', '战列舰', '潜艇', '舰', '船', '艇'), '海军'),
    (('战车', '坦克', '机甲', '矿机', '采矿', '机车', '装甲', '歼击车', '导弹车',
      '火箭车', '发射车', '突击炮', '粉碎者'), '载具'),
    (('步兵', '工兵', '军团长', '指挥官'), '步兵'),
    (('塔', '厂', '基地', '站', '中心', '炮台', '要塞', '机场', '停机坪', '核心',
      '礼炮', '雅典娜炮'), '建筑'),
]
TYPE_HINTS = ('Structure', 'Production', 'Defence', 'Defense', 'Perimeter', 'Facility')


def classify_type(uid, type_line='', cn_name=''):
    # ① 官方单位图鉴的 kind_of —— **权威**（隔壁会话从 cor-games.com/units 抓的，
    #    字段就是 SAGE 原生的类型标志位）。顺序 = 优先级：军舰/飞机同时也带 VEHICLE，
    #    所以子类必须先判，否则航母又会被判成"载具"。
    if ODB:
        r = ODB.record(uid)
        if r:
            ks = set(r.get('kind_of') or [])
            for f, c in OFFICIAL_KIND:
                if f in ks:
                    return c
    # ② 命名启发式兜底（对象不在图鉴库里时）
    s = uid or ''
    # ① 建筑词。注意两点：
    #    - **大小写敏感**匹配（驼峰词才可信）——否则 "Sup**port**"/"Trans**port**" 会命中 "Port"；
    #    - 跳过 "AntiXXX" 前缀 —— `CelestialAnti**Structure**Ship` 是"反建筑**舰**"，
    #      说的是目标不是自身，必须让它继续走到 ② 的 "Ship"。
    for kw in BUILD_WORDS:
        p = s.find(kw)
        while p >= 0:
            if s[max(0, p - 4):p].lower() != 'anti':
                return '建筑'
            p = s.find(kw, p + 1)
    # ② 兵种词：取名字里**最后出现**的那个
    #    （CelestialAntiInfantry**Vehicle** → 载具；AlliedGaintAirCraft**Carrier** → 海军）
    cat, pos = None, -1
    for kw, c in RANGED_WORDS:
        p = s.rfind(kw)
        if p > pos:
            pos, cat = p, c
    if cat:
        return cat
    cn = cn_name or ''
    for words, c in CN_KINDS:                  # ③ 中文名兜底
        for w in words:
            if w in cn:
                return c
    for kw in TYPE_HINTS:                      # ④ TYPE: 描述
        if kw in (type_line or ''):
            return '建筑'
    return '其他'


# ---------------- 箱子中文名（按对象模板名翻译） ----------------
CRATE_CN = {
    'HealCrateMP': '治疗箱·多人',
    'MoneyCrateMP': '现金箱·多人',
    'MoneyCrate_200': '现金箱·200',
    'VeterancyCrateMP': '晋升箱·多人',

    'AreaOfEffectDownAttributeCrate': '属性箱·作用半径↓',
    'AreaOfEffectUpAttributeCrate': '属性箱·作用半径↑',
    'ArmorDownAttributeCrate': '属性箱·护甲↓',
    'ArmorUpAttributeCrate': '属性箱·护甲↑',
    'AttackSuperDownAttributeCrate': '属性箱·攻击力大幅↓',
    'AttackSuperUpAttributeCrate': '属性箱·攻击力大幅↑',
    'AttributeCrate': '属性箱·随机',
    'BaseAttributeCrate': '属性箱·基础',
    'CantAttackAttributeCrate': '属性箱·无法攻击',
    'CantMoveAttributeCrate': '属性箱·无法移动',
    'DamageMultDownAttributeCrate': '属性箱·伤害倍率↓',
    'DamageMultUpAttributeCrate': '属性箱·伤害倍率↑',
    'DeflectIncomingFireBoxAttributeCrate': '属性箱·弹开来袭火力',
    'EmotionDissidentAttributeCrate': '属性箱·煽动叛变',
    'HealSuperDownAttributeCrate': '属性箱·治疗大幅↓',
    'HealSuperUpAttributeCrate': '属性箱·治疗大幅↑',
    'InaudibleAttributeCrate': '属性箱·静默无声',
    'IronCurtainAttributeCrate': '属性箱·铁幕',
    'NoSpecialAbilityAttributeCrate': '属性箱·禁用特殊能力',
    'RangeDownAttributeCrate': '属性箱·射程↓',
    'RangeSuperDownAttributeCrate': '属性箱·射程大幅↓',
    'RangeSuperUpAttributeCrate': '属性箱·射程大幅↑',
    'RangeUpAttributeCrate': '属性箱·射程↑',
    'RateOfFireDownAttributeCrate': '属性箱·射速↓',
    'RateOfFireUpAttributeCrate': '属性箱·射速↑',
    'RechargeTimeDownAttributeCrate': '属性箱·充能缩短',
    'RechargeTimeUpAttributeCrate': '属性箱·充能延长',
    'ShroudRevealToAllAttributeCrate': '属性箱·向全体揭示',
    'SpeedDownAttributeCrate': '属性箱·速度↓',
    'SpeedSuperDownAttributeCrate': '属性箱·速度大幅↓',
    'SpeedSuperUpAttributeCrate': '属性箱·速度大幅↑',
    'SpeedUpAttributeCrate': '属性箱·速度↑',

    'HealCrate': '治疗箱',
    'BaseBombCrate': '炸弹箱·基地炸弹',
    'BombCrate': '炸弹箱',
    'EmotionDissidentBombCrate': '炸弹箱·煽动叛变',
    'ForzenCrate': '炸弹箱·冰冻',
    'NanoPointDefenceCrate': '炸弹箱·纳米点防御',
    'ParticleCannonCrate': '炸弹箱·粒子炮',
    'RadarCloseBombCrate': '炸弹箱·雷达干扰',
    'SuperBombCrate': '炸弹箱·超级炸弹',
    'SuperMetaImpactCrate': '炸弹箱·超级金属冲击',
    'TelekineticBombCrate': '炸弹箱·念力',
    'VacuumBombCrate': '炸弹箱·真空弹',

    'MoneyCrate': '现金箱',
    'ShroudCrate': '监视箱·揭示视野',
    'RandomCrate': '神秘箱·随机奖励',
    'VeterancyCrate': '晋升箱',
    'UnitCrate': '部队箱',
    'UnitCrateNew': '部队箱·新版',
}

# 兼容箱子行的两种写法：
#   A) { u8"阵营", u8"名字", "模板名", 0x..u, 1 },
#   B) { u8"阵营", u8"类型", u8"名字", "模板名", "拼音", 0x..u, 1 },
CRATE_RE = re.compile(
    r'^\s*\{\s*u8"(?P<f>[^"]*)"\s*,\s*'
    r'(?:u8"(?P<t>[^"]*)"\s*,\s*)?'
    r'u8"(?P<n>[^"]*)"\s*,\s*'
    r'"(?P<id>[^"]*)"\s*,\s*'
    r'(?:"[^"]*"\s*,\s*)?'
    r'(?P<h>0x[0-9A-Fa-f]{8})u\s*(?:,\s*\d+)?\s*\}\s*,?\s*$')


def esc(s):
    return s.replace('\\', '\\\\').replace('"', '\\"')


def read_crates():
    """从现有头文件里抓箱子（哈希沿用已验证数据，显示名换成中文）"""
    if not os.path.exists(OUT):
        return []
    txt = io.open(OUT, encoding='utf-8').read()
    m = re.search(r'static const SupportUnit kCrates\[\]\s*=\s*\{(.*?)\n\};', txt, re.S)
    if not m:
        return []
    out = []
    for line in m.group(1).splitlines():
        mm = CRATE_RE.match(line)
        if mm:
            out.append((mm.group('f'), mm.group('id'), mm.group('h')))
    return out


def main():
    crates = read_crates()
    print('[*] 箱子条目: %d' % len(crates))
    if not crates:
        print('[x] 没能从现有 units_corona.h 抓出 kCrates')
        return 1

    src = FINAL_TSV if os.path.exists(FINAL_TSV) else TSV
    rows_in = list(csv.DictReader(io.open(src, encoding='utf-8'), delimiter='\t'))
    print('[*] 源文件 %s：%d 条' % (os.path.basename(src), len(rows_in)))

    # TYPE: 描述只在 trainer.tsv 里有（callable_final 没这列），拿来辅助判类型
    tline_map = {}
    if os.path.exists(TSV):
        for tr in csv.DictReader(io.open(TSV, encoding='utf-8'), delimiter='\t'):
            tline_map[tr.get('name', '')] = (tr.get('type_line') or '').strip()

    rows = []
    for r in rows_in:
        uid = (r.get('name') or r.get('id') or '').strip()
        code = (r.get('code_hex') or '').strip()
        if not uid or not code:
            continue
        fac = (r.get('faction') or '').strip()
        if fac == '中立':
            continue
        prim = str(r.get('is_primary') or r.get('primary') or '0').strip()
        rows.append(dict(faction=fac,
                         chinese_name=(r.get('chinese_name') or '').strip(),
                         name=uid, code_hex=code,
                         primary=1 if prim == '1' else 0))

    # ---- 先算好「类型」和「显示名」，再排序：**类型 → 阵营 → 名称** ----
    # （需求：同类要聚在一起，别建筑和单位混着排、翻起来找不到）
    TYPE_ORDER = ['建筑', '步兵', '载具', '空军', '海军', '箱子', '其他']
    for r in rows:
        r['type'] = classify_type(r['name'], tline_map.get(r['name'], ''), r['chinese_name'])
        r['disp'] = official_name(r['name']) or (r['chinese_name'] or '').strip() or r['name']
    order_key = {f: i for i, f in enumerate(FACTION_ORDER)}
    tkey = {t: i for i, t in enumerate(TYPE_ORDER)}
    rows.sort(key=lambda r: (tkey.get(r['type'], 99), order_key.get(r['faction'], 99), r['disp']))

    seen = set()
    lines = []
    n_primary = 0
    n_unofficial = 0
    n_official = 0
    type_count = {}
    for r in rows:
        name = r['disp']                        # official / 中文名 / 模板名，三者取一（已算好）
        if official_name(r['name']):
            n_official += 1
        elif not has_cjk((r['chinese_name'] or '').strip()):
            # 既没官方名、又没中文名 → 用按功能意译的名字，并标注 [非官方]
            name = (UNIT_CN.get(r['name']) or name or r['name']) + UNOFFICIAL
            n_unofficial += 1
        key = (r['faction'], r['name'])
        if key in seen:
            continue
        seen.add(key)
        if r['primary']:
            n_primary += 1
        t = r['type']
        type_count[t] = type_count.get(t, 0) + 1
        lines.append('    { u8"%s", u8"%s", u8"%s", "%s", "%s", %su, %d },'
                     % (esc(r['faction']), esc(t), esc(name), esc(r['name']), esc(py_of(name)),
                        r['code_hex'], r['primary']))

    facs = [f for f in FACTION_ORDER if any(r['faction'] == f for r in rows)]

    crate_lines = []
    for fac, uid, code in crates:
        zh = CRATE_CN.get(uid, uid)
        crate_lines.append('    { u8"%s", u8"箱子", u8"%s", "%s", "%s", %su, 1 },'
                           % (esc(fac), esc(zh), esc(uid), esc(py_of(zh)), code))

    head = '''// units_corona.h - 日冕单位 / 箱子表（自动生成，请勿手改）
// 生成器 : ra3-copilot/tools/gen_units_corona.py
// 单位源 : unit-codes/data/callable_final.tsv
//          = ① ThingFactory::Find 实测「模板存在、能创建」∩ ② 官方图鉴 kind_of 含 SELECTABLE
//            「创建出来能选中、能操作」（缺 SELECTABLE 的如 AlliedChronoBeacon_Poseidon 已剔除，
//             它们能 resolve 但召唤出来不可选、不能用）。已排除中立阵营。
// 剔除留档 : unit-codes/data/callable_dropped.tsv（每条带原因）
// 箱子   : 54 条（哈希沿用旧表；显示名按对象模板名译成中文）
// 字段   : 阵营 / **类型** / 中文名 / 对象模板名 / 拼音(首字母 全拼) / 对象类型哈希 / 是否基准形态
// 类型   : 建筑·步兵·载具·空军·海军·箱子·其他 —— 按命名规则推断（SAGE 的权威分类是模板里的
//          KindOf 位掩码，需运行时读；这里先用命名规则，取名字里**最后出现的兵种词**）
// 拼音   : pypinyin 预计算写死，运行时零依赖 —— UI 输入 "jdc" 即可搜到「基地车」
// 说明   : 哈希 = StringHasher.Hash(模板名.lower())，可离线重算（见 unit-codes/tools/sage_hash.py）
#pragma once
#include <cstdint>

namespace ra3 {

struct SupportUnit {
    const char* faction;   // 阵营
    const char* type;      // 类型（建筑/步兵/载具/空军/海军/箱子/其他）
    const char* name;      // 显示名（中文；无中文名时是按功能意译并标 [非官方]）
    const char* id;        // 对象模板名
    const char* py;        // 拼音（"jdc jidiche"，空格分隔首字母与全拼；非中文为空串）
    uint32_t    hash;      // 对象类型哈希
    uint8_t     primary;   // 1 = 基准形态
};

// 阵营展示顺序（支援面板左侧栏用，"全部"与"箱子"是 UI 的额外页签）
static const char* const kSupportFactions[] = { %s };
static const int kSupportFactionCount = %d;

static const SupportUnit kSupportUnits[] = {
%s
};
static const int kSupportUnitCount = %d;

// 箱子（拾取类奖励物，全部按基准形态处理）
static const SupportUnit kCrates[] = {
%s
};
static const int kCrateCount = %d;

} // namespace ra3
''' % (', '.join('"%s"' % f for f in facs), len(facs),
       '\n'.join(lines), len(lines),
       '\n'.join(crate_lines), len(crate_lines))

    io.open(OUT, 'w', encoding='utf-8', newline='\n').write(head)
    print('[ok] 写出 %s' % OUT)
    print('     单位 %d 条（基准形态 %d，官方名 %d 条，仍未译 %d 条） / 箱子 %d 条 / 阵营 %s'
          % (len(lines), n_primary, n_official, n_unofficial, len(crate_lines), facs))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
