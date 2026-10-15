"""official_db.py — 官方单位图鉴数据库访问器（cor-games.com/units 的数据源）

数据文件：../data/source/game_objects.jsonl
  下载：https://cor-games.com/units/ 页面里 fetch 的是
        https://cor-games.com/data/game_objects/game_objects.jsonl

关键字段
  editor_name    对象模板名（= 算哈希用的那个名字）
  display_name   官方中文名（最权威）
  side           阵营：Celestial / ALLIES / SOVIET / JAPAN / Neutral / Civilian
  editor_sorting unit / structure / campaign_units / campaign_structures / system /
                 misc_man_made / destructible_props / optimized_props / misc_natural / none
  kind_of        类型标志位列表（SELECTABLE / INFANTRY / VEHICLE / SHIP / AIRCRAFT /
                 STRUCTURE / IMMOBILE / CRATE / IGNORES_SELECT_ALL / TRANSPORT / SUMMONED …）
  logic_commands 可用指令（含 Command_Construct* 的说明是可建造对象）

判据：`SELECTABLE` ∈ kind_of  → 玩家能选中、召唤出来能正常用
  缺 `SELECTABLE` 的都是「召唤出来也用不了」的东西，典型：
    AlliedChronoBeacon_Poseidon   超时空传送抵达体（IGNORES_SELECT_ALL）
    CelestialAirDrop_Land         空投运输体（TRANSPORT）
    SovietTeslaAirAssault         被召唤的运输飞行器（SUMMONED + IGNORES_SELECT_ALL）
    AlliedPowerPlantTurbine       建筑子装置（UNATTACKABLE + SUMMONED）
"""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(os.path.abspath(os.path.join(HERE, '..')), 'data', 'source')
JSONL = os.path.join(SRC, 'game_objects.jsonl')

# 修改器要的类别（真正的单位 / 建筑 / 战役单位）
TRAINER_CATEGORIES = ('unit', 'structure', 'campaign_units', 'campaign_structures')

_cache = None


def load():
    """{editor_name.lower(): record}（同名多条时保留信息最全的一条）"""
    global _cache
    if _cache is not None:
        return _cache
    out = {}
    if os.path.exists(JSONL):
        for line in open(JSONL, encoding='utf-8'):
            line = line.strip()
            if not line:
                continue
            r = json.loads(line)
            k = (r.get('editor_name') or '').lower()
            if not k:
                continue
            old = out.get(k)
            if old is None or len(r.get('kind_of', [])) > len(old.get('kind_of', [])):
                out[k] = r
    _cache = out
    return out


def record(name):
    return load().get((name or '').lower())


def has(rec, flag):
    return bool(rec) and flag in rec.get('kind_of', [])


def is_usable(name):
    """True = 可选中可用；False = 官方库明确标记为不可选中；None = 不在官方库，无法判定。"""
    rec = record(name)
    if rec is None:
        return None
    return 'SELECTABLE' in rec.get('kind_of', [])


def is_trainer_object(name):
    """是否是"真正的单位/建筑"（按官方分类），与是否可选中无关。"""
    rec = record(name)
    if rec is None:
        return None
    return rec.get('editor_sorting') in TRAINER_CATEGORIES


def reason_unusable(name):
    """给一条人类可读的"为什么不能用"。"""
    rec = record(name)
    if rec is None:
        return '不在官方图鉴库中，无法核验'
    flags = set(rec.get('kind_of', []))
    if 'SELECTABLE' in flags:
        return ''
    for f, why in (('IGNORES_SELECT_ALL', '忽略全部选择（传送/空投抵达体）'),
                   ('SUMMONED', '由技能/超武召唤产生的临时体'),
                   ('TRANSPORT', '运输/搭载体'),
                   ('UNATTACKABLE', '不可攻击的子装置')):
        if f in flags:
            return why
    return '官方库未标记 SELECTABLE'


if __name__ == '__main__':
    import sys
    db = load()
    print('官方图鉴条目 %d' % len(db))
    for n in sys.argv[1:]:
        r = record(n)
        print('%-40s %s  %s' % (
            n, r['display_name'] if r else '(不在库)',
            '可用' if is_usable(n) else (reason_unusable(n) or '?')))
