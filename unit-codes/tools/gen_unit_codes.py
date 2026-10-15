"""Generate the RA3 / 日冕 object-template (unit) code tables.

Source files (all under ../data/source/)
  game_objects.jsonl       官方单位图鉴数据 (cor-games.com/units → /data/game_objects/game_objects.jsonl)
                           ★ 权威：editor_name = 对象模板名, display_name = 中文名, side = 阵营,
                             kind_of = 类型标志位, editor_sorting = unit/structure/prop 分类
  worldbuilder.manifest    WBData.big     -> 原版对象调色板符号表
  mod.manifest             Core13.big     -> 补丁 / Mod 符号表
  data_mod.str             Cor_ENG.big    -> NAME/DESC/TYPE  (驼峰键；英文包)
  gamestrings_cn.csf       Lang-ChineseT13.big -> NAME/DESC/TYPE (全大写键；中文)
Ground truth
  ../../trainer-v2/src/units_corona.h  -> 436 条实机验证过的 名字 -> 代码

Code algorithm
  code = StringHasher.Hash(name.lower())     (SuperFastHash 变体)
  出处: WrathEd2012 / SAGE/StringHasher.cs

"能不能正常召唤出来用"的判据
  kind_of 含 SELECTABLE  -> 玩家可选中/可操作 = 召唤出来是能用的
  缺 SELECTABLE 的（如 AlliedChronoBeacon_Poseidon）会生成对象但不可选、不可用。
"""
import os, re, json, sys, collections

HERE = os.path.dirname(os.path.abspath(__file__))
ARCHIVE = os.path.abspath(os.path.join(HERE, '..'))
sys.path.insert(0, HERE)
from sage_hash import sage_hash

SRC = os.path.join(ARCHIVE, 'data', 'source')
DATA = os.path.join(ARCHIVE, 'data')
ROOT = os.path.abspath(os.path.join(ARCHIVE, '..'))
CORONA_H = os.path.join(ROOT, 'trainer-v2', 'src', 'units_corona.h')

TYPE_ALT = (r'GameObject|WeaponTemplate|UpgradeTemplate|SpecialPowerTemplate|ObjectCreationList|'
            r'FXList|LocomotorTemplate|ArmorTemplate|PlayerTemplate|FXParticleSystemTemplate|'
            r'AudioEvent|AudioFile|Multisound|ExperienceLevelTemplate|AttributeModifier|DamageFX|'
            r'LogicCommand|LogicCommandSet|PlayerTech|Science|AIPersonalityDefinition|'
            r'AIBuildStateDefinition|W3DMesh|W3DContainer|W3DAnimation|W3DHierarchy|'
            r'W3DCollisionBox|Texture|PackedTextureImage|ReticleTemplate|TerrainTemplate|'
            r'RoadTemplate|Road|WaterTextureList|MappedImage|Font|Scorch|SabotageHorde')
TOKEN = re.compile(rb'(?<![A-Za-z0-9_:])(' + TYPE_ALT.encode() + rb'[A-Za-z0-9_]*):([A-Za-z0-9_.\-]+)')

# 修改器要的类别：真正的单位 / 建筑 / 战役单位
TRAINER_CATEGORIES = ('unit', 'structure', 'campaign_units', 'campaign_structures')

SIDE_CN = {'Celestial': '神州', 'ALLIES': '盟军', 'Allied': '盟军', 'SOVIET': '苏联',
           'JAPAN': '帝国', 'Neutral': '中立', 'Civilian': '平民', '0': '特殊'}
FAC_ORDER = {'神州': 0, '盟军': 1, '苏联': 2, '帝国': 3, '中立': 4, '平民': 5, '特殊': 6}
CLASS_ORDER = {'建筑': 0, '步兵': 1, '载具': 2, '舰船': 3, '飞行器': 4, '箱子': 5, '其他': 6}
# 放进 flags 列的关键标志位（完整 kind_of 在 JSON 里）
FLAG_KEEP = ('IMMOBILE', 'CAN_ATTACK', 'AMPHIBIOUS', 'TRANSFORMER', 'GARRISON', 'CRATE',
             'UNATTACKABLE', 'NOT_AUTOACQUIRABLE', 'WALL', 'POWERED', 'UNPACKS_INTO_BUILDING')


# ---------------------------------------------------------------- 来源解析

def parse_symbols(path):
    d = open(path, 'rb').read()
    out = collections.defaultdict(set)
    for m in TOKEN.finditer(d):
        out[m.group(1).decode()].add(m.group(2).decode())
    return out


def parse_mod_str(path):
    out = {}
    d = open(path, encoding='utf-8', errors='replace').read()
    for m in re.finditer(r'^NAME:([A-Za-z0-9_\-\.]+)\s*\n\s*"((?:[^"\\]|\\.)*)"', d, re.M):
        out.setdefault(m.group(1).lower(), m.group(2))
    return out


def _csf(path):
    from csf_read2 import parse as csf_parse
    old = sys.argv
    sys.argv = [old[0], path, '', 'chars']
    try:
        return csf_parse(path)[0]
    finally:
        sys.argv = old


def parse_csf_cn(path):
    out = {}
    for k, v in _csf(path).items():
        if k.startswith('NAME:') and not k.startswith('NAME:ABILITY'):
            out.setdefault(k[5:].lower(), v)
    return out


def load_loc_signals():
    """{name_lower: {'NAME','DESC','TYPE'}} —— 用于基准形态判定与补名。"""
    sig = collections.defaultdict(set)
    p = os.path.join(SRC, 'data_mod.str')
    if os.path.exists(p):
        d = open(p, encoding='utf-8', errors='replace').read()
        for m in re.finditer(r'^(NAME|DESC|TYPE):([A-Za-z0-9_\-\.]+)\s*\n', d, re.M):
            sig[m.group(2).lower()].add(m.group(1))
    p = os.path.join(SRC, 'gamestrings_cn.csf')
    if os.path.exists(p):
        for k in _csf(p):
            if ':' in k:
                t, v = k.split(':', 1)
                if t in ('NAME', 'DESC', 'TYPE'):
                    sig[v.lower()].add(t)
    return sig


NON_OBJECT = re.compile(
    r'^(Activate|Deactivate|Toggle|Pause|Resume|PowerSeal|SetAuto|Select|Clear|Upgrade_|'
    r'CAMPAIGNPLAYERPOWER)', re.I)


def load_typed_objects():
    """有 `TYPE:` 键（信息面板的"类型"行）= 真·可建造/可操作对象，用来补原版阵营建筑。"""
    out = {}
    p = os.path.join(SRC, 'data_mod.str')
    if os.path.exists(p):
        d = open(p, encoding='utf-8', errors='replace').read()
        names, types = {}, {}
        for m in re.finditer(r'^NAME:([A-Za-z0-9_\-\.]+)\s*\n\s*"((?:[^"\\]|\\.)*)"', d, re.M):
            names.setdefault(m.group(1), m.group(2))
        for m in re.finditer(r'^TYPE:([A-Za-z0-9_\-\.]+)\s*\n\s*"((?:[^"\\]|\\.)*)"', d, re.M):
            types.setdefault(m.group(1), m.group(2))
        for n, t in types.items():
            if not NON_OBJECT.match(n):
                out[n] = {'cn': names.get(n, ''), 'type': t}
    p = os.path.join(SRC, 'gamestrings_cn.csf')
    if os.path.exists(p):
        strs = _csf(p)
        names, types = {}, {}
        for k, v in strs.items():
            if k.startswith('NAME:') and not k.startswith('NAME:ABILITY'):
                names.setdefault(k[5:], v)
            elif k.startswith('TYPE:'):
                types.setdefault(k[5:], v)
        for n, t in types.items():
            if not NON_OBJECT.match(n):
                out.setdefault(n, {'cn': names.get(n, ''), 'type': t})
    return out


def load_official():
    """官方单位图鉴数据 → {editor_name: record}"""
    p = os.path.join(SRC, 'game_objects.jsonl')
    if not os.path.exists(p):
        print('[warn] game_objects.jsonl 不存在，跳过官方库')
        return {}
    out = {}
    for line in open(p, encoding='utf-8'):
        line = line.strip()
        if not line:
            continue
        r = json.loads(line)
        out[r['editor_name']] = r
    return out


# ---------------------------------------------------------------- 小工具

def split_camel(name):
    parts = []
    for chunk in name.split('_'):
        cur, prev = '', ''
        for ch in chunk:
            if cur and ((ch.isupper() and prev.islower())
                        or (ch.isdigit() and not prev.isdigit())
                        or (ch.isalpha() and prev.isdigit())):
                parts.append(cur)
                cur = ''
            cur += ch
            prev = ch
        if cur:
            parts.append(cur)
    return parts


def build_vocab(names):
    v = {}
    for n in names:
        for w in split_camel(n):
            if len(w) >= 2:
                v.setdefault(w.lower(), w)
    return v


def recase(name, vocab, maxlen=16):
    """全大写名尽量还原驼峰（哈希不区分大小写，只影响显示；切不干净就原样返回）。"""
    if not name.isupper():
        return name
    low, out, i = name.lower(), [], 0
    while i < len(low):
        best = None
        for L in range(min(maxlen, len(low) - i), 1, -1):
            w = vocab.get(low[i:i + L])
            if w:
                best = (L, w)
                break
        if not best:
            return name
        out.append(best[1])
        i += best[0]
    return ''.join(out)


def build_families(names):
    ns = set(names)
    root = {}
    for n in ns:
        cur, changed = n, True
        while changed:
            changed = False
            i = cur.rfind('_')
            while i > 0:
                cand = cur[:i]
                if cand in ns:
                    cur, changed = cand, True
                    break
                i = cur.rfind('_', 0, i)
        root[n] = cur
    return root


def classify(n, has_name):
    if has_name:
        return 'unit'
    if re.match(r'^(A|S|J)\d\d_', n):
        return 'campaign'
    if re.match(r'^[A-Z]{2}_', n):
        return 'scenery'
    low = n.lower()
    if 'collapse' in low or 'debris' in low or 'rubble' in low:
        return 'wreck'
    if 'projectile' in low:
        return 'projectile'
    if 'effect' in low or 'spawner' in low or low.startswith('fx'):
        return 'fx'
    if 'bridge' in low:
        return 'bridge'
    return 'misc'


KIND_CN = {'unit': '可玩对象(单位/建筑)', 'scenery': '地图场景/中立物', 'campaign': '战役专用',
           'wreck': '残骸/倒塌态', 'projectile': '投射物', 'fx': '特效对象',
           'bridge': '桥部件', 'misc': '其他'}


def unit_class(rec):
    if rec is None:
        return ''
    cat = rec['editor_sorting']
    s = set(rec['kind_of'])
    if cat.startswith('structure'):
        return '建筑'
    if 'CRATE' in s:
        return '箱子'
    if 'INFANTRY' in s:
        return '步兵'
    if 'AIRCRAFT' in s:
        return '飞行器'
    if 'SHIP' in s:
        return '舰船'
    if 'VEHICLE' in s:
        return '载具'
    return '其他'


# ---------------------------------------------------------------- 主流程

def main():
    wb = parse_symbols(os.path.join(SRC, 'worldbuilder.manifest'))
    mo = parse_symbols(os.path.join(SRC, 'mod.manifest'))
    go_wb = set(wb.get('GameObject', set()))
    go = go_wb | set(mo.get('GameObject', set()))

    hdr = open(CORONA_H, encoding='utf-8').read()
    # 兼容两代 units_corona.h 的条目格式
    corona = []
    for m in re.finditer(r'\{ u8"([^"]*)", u8"([^"]*)", u8"([^"]*)", "([^"]+)", (?:u8")?[^"]*"?,?\s*(0x[0-9A-Fa-f]+)u?', hdr):
        corona.append({'faction': m.group(1), 'cn': m.group(3), 'name': m.group(4),
                       'code': int(m.group(5), 16)})
    if not corona:
        for m in re.finditer(r'\{ u8"([^"]*)", u8"([^"]*)", "([^"]+)", (0x[0-9A-Fa-f]+)u \}', hdr):
            corona.append({'faction': m.group(1), 'cn': m.group(2), 'name': m.group(3),
                           'code': int(m.group(4), 16)})
    # 运行时实测过的 名字->代码（更硬的真值）
    runtime = []
    p_cf = os.path.join(DATA, 'callable_final.tsv')
    if os.path.exists(p_cf):
        for line in open(p_cf, encoding='utf-8').read().splitlines()[1:]:
            c = line.split('\t')
            if len(c) >= 4 and c[3].startswith('0x'):
                runtime.append((c[2], int(c[3], 16)))
    for s, h in runtime:
        if all(x['name'] != s for x in corona):
            corona.append({'faction': '', 'cn': '', 'name': s, 'code': h})
    ok = sum(1 for c in corona if sage_hash(c['name'].lower()) == c['code'])
    print(f'[verify] SAGE hash 复算 {ok}/{len(corona)} 条已验证的 名字->代码')

    official = load_official()
    print(f'[names]  官方单位图鉴 editor_name = {len(official)}')

    cn_mod = parse_mod_str(os.path.join(SRC, 'data_mod.str'))
    try:
        cn_base = parse_csf_cn(os.path.join(SRC, 'gamestrings_cn.csf'))
    except Exception as e:
        print('[warn] csf parse failed:', e)
        cn_base = {}
    sig = load_loc_signals()

    fac_corona = {c['name']: c['faction'] for c in corona}
    cn_corona = {c['name']: c['cn'] for c in corona}
    FAC_PREFIX = [('Allied', '盟军'), ('Soviet', '苏联'), ('Japan', '帝国'), ('Celestial', '神州')]

    typed = load_typed_objects()
    vocab = build_vocab(go | set(fac_corona) | set(official))
    extra, extra_cn, extra_type = set(), {}, {}
    for raw, info in typed.items():
        proper = recase(raw, vocab)
        if proper.lower() in {x.lower() for x in go} or proper.lower() in {x.lower() for x in fac_corona}:
            continue
        if proper in extra:
            continue
        extra.add(proper)
        extra_cn[proper] = info['cn']
        extra_type[proper] = info['type']
    print(f'[names]  TYPE: 补充对象 = {len(extra)}')

    # 按小写名去重（哈希不区分大小写），大小写按来源权威度取最优写法
    cand = set(go) | set(fac_corona) | extra | set(official)
    best = {}
    for n in cand:
        k = n.lower()
        score = (0 if n in official else 1 if n in fac_corona else 2 if n in go else 3,
                 1 if n.isupper() else 0)
        if k not in best or score < best[k][0]:
            best[k] = (score, n)
    allnames = sorted(v[1] for v in best.values())
    deduped = len(cand) - len(allnames)
    if deduped:
        print(f'[names]  大小写重复合并 {deduped} 条')

    def pick_cn(*cands):
        for c in cands:
            if c and re.search(r'[\u4e00-\u9fff]', c):
                return c
        for c in cands:
            if c:
                return c
        return ''

    fam = build_families(allnames)
    fam_members = collections.defaultdict(list)
    for n in allnames:
        fam_members[fam[n]].append(n)

    primary, reason = {}, {}
    for r, members in fam_members.items():
        withdesc = [n for n in members if 'DESC' in sig.get(n.lower(), set())]
        if len(withdesc) == 1:
            primary[withdesc[0]] = True
            reason[withdesc[0]] = '唯一 DESC(可建造面板)'
        elif len(withdesc) > 1:
            for n in withdesc:
                primary[n] = True
                reason[n] = '多 DESC(阵营/真单位同名)'
        else:
            primary[r] = True
            reason[r] = '无 DESC→取最短名前缀'

    rows = []
    for n in allnames:
        code = sage_hash(n.lower())
        rec = official.get(n)
        cnname = pick_cn(rec['display_name'] if rec else None,
                         cn_corona.get(n), extra_cn.get(n),
                         cn_mod.get(n.lower()), cn_base.get(n.lower()))
        if rec:
            fac = SIDE_CN.get(str(rec['side']), '')
            db_cat = rec['editor_sorting']
            flags = rec['kind_of']
            summonable = 'yes' if 'SELECTABLE' in flags else 'no'
            kind = 'unit' if (summonable == 'yes' and db_cat in TRAINER_CATEGORIES) else 'misc'
        else:
            fac = fac_corona.get(n) or next((f for p, f in FAC_PREFIX if n.startswith(p)), '中立')
            db_cat, flags, summonable = '', [], ''
            kind = 'unit' if n in extra else classify(n, bool(cnname))
        rows.append({
            'name': n, 'code': '0x%08X' % code, 'code_dec': code,
            'faction': fac, 'chinese': cnname,
            'family': fam[n], 'primary': '1' if primary.get(n) else '0',
            'why': reason.get(n, '变体/形态(可由基准派生)' if primary.get(fam[n]) else ''),
            'kind': kind, 'kind_cn': KIND_CN.get(kind, '其他'),
            'type_line': extra_type.get(n, ''),
            'summonable': summonable,
            'db_cat': db_cat,
            'unit_class': unit_class(rec),
            'flags': '|'.join(f for f in flags if f in FLAG_KEEP) if rec else '',
            'kind_of': flags if rec else [],
            'logic_commands': rec.get('logic_commands', []) if rec else [],
            'button': rec.get('button_image_name', '') if rec else '',
            'in_official': 'y' if rec else '',
            'src': ('wb' if n in go_wb else '') + ('+mod' if n in mo.get('GameObject', set()) else '')
                   + ('+corona' if n in fac_corona else '') + ('+typed' if n in extra else '')
                   + ('+official' if rec else ''),
        })

    # ---------------- 全量表 ----------------
    tsv = os.path.join(DATA, 'unit_codes_all.tsv')
    with open(tsv, 'w', encoding='utf-8', newline='\n') as f:
        f.write('code_hex\tcode_dec\tname\tfaction\tchinese_name\tfamily\tis_primary\tnote\t'
                'type_line\tkind\tkind_cn\tsummonable\tdb_category\tunit_class\tflags\tsource\n')
        for x in rows:
            f.write(f"{x['code']}\t{x['code_dec']}\t{x['name']}\t{x['faction']}\t{x['chinese']}\t"
                    f"{x['family']}\t{x['primary']}\t{x['why']}\t{x['type_line']}\t"
                    f"{x['kind']}\t{x['kind_cn']}\t"
                    f"{x['summonable']}\t{x['db_cat']}\t{x['unit_class']}\t{x['flags']}\t{x['src']}\n")
    json.dump(rows, open(os.path.join(DATA, 'unit_codes_all.json'), 'w', encoding='utf-8'),
              ensure_ascii=False, indent=1)

    # ---------------- 修改器表：只保留"召唤出来能正常用"的对象 ----------------
    units = [x for x in rows if x['summonable'] == 'yes' and x['db_cat'] in TRAINER_CATEGORIES]
    units.sort(key=lambda x: (FAC_ORDER.get(x['faction'], 9),
                              CLASS_ORDER.get(x['unit_class'], 9),
                              0 if x['primary'] == '1' else 1,
                              x['name'].lower()))

    t_tsv = os.path.join(DATA, 'unit_codes_trainer.tsv')
    with open(t_tsv, 'w', encoding='utf-8', newline='\n') as f:
        f.write('code_hex\tcode_dec\tname\tfaction\tchinese_name\ttype_line\tfamily\t'
                'is_primary\tnote\tcategory\tunit_class\tflags\n')
        for x in units:
            f.write(f"{x['code']}\t{x['code_dec']}\t{x['name']}\t{x['faction']}\t{x['chinese']}\t"
                    f"{x['type_line']}\t{x['family']}\t{x['primary']}\t{x['why']}\t"
                    f"{x['db_cat']}\t{x['unit_class']}\t{x['flags']}\n")

    json.dump(units, open(os.path.join(DATA, 'unit_codes_trainer.json'), 'w', encoding='utf-8'),
              ensure_ascii=False, indent=1)

    def cstr(s):
        return s.replace('\\', '\\\\').replace('"', '\\"')

    t_h = os.path.join(DATA, 'unit_codes_trainer.h')
    with open(t_h, 'w', encoding='utf-8', newline='\n') as f:
        f.write('// unit_codes_trainer.h - RA3 日冕 修改器单位代码表（自动生成，勿手改）\n')
        f.write('// 生成器 : unit-codes/tools/gen_unit_codes.py\n')
        f.write('// 代码   : code = StringHasher.Hash(对象模板名.lower())  —— SuperFastHash 变体\n')
        f.write('// 验证   : 436/436 实机数据 + 官方单位图鉴(editor_name) 交叉核对\n')
        f.write('// 收录   : 官方库 kind_of 含 SELECTABLE 的 unit/structure —— 可选中、召唤出来能正常用\n')
        f.write('//          （缺 SELECTABLE 的如 AlliedChronoBeacon_Poseidon 已排除）\n')
        f.write(f'// 条目   : {len(units)}（基准形态 {sum(1 for x in units if x["primary"] == "1")} 条）\n')
        f.write('#pragma once\n#include <cstdint>\n\nnamespace ra3 {\n\n')
        f.write('struct TrainerUnit { const char* faction; const char* name; const char* id;'
                ' const char* cls; uint32_t code; uint8_t primary; };\n\n')
        f.write('static const TrainerUnit kTrainerUnits[] = {\n')
        for x in units:
            f.write('    { u8"%s", u8"%s", "%s", u8"%s", %su, %s },\n' % (
                cstr(x['faction']), cstr(x['chinese'] or x['name']), cstr(x['name']),
                cstr(x['unit_class']), x['code'], x['primary']))
        f.write('};\n\n')
        f.write(f'static const int kTrainerUnitCount = {len(units)};\n\n')
        f.write('} // namespace ra3\n')

    # ---------------- 符号表快照 ----------------
    with open(os.path.join(DATA, 'wb_symbols.json'), 'w', encoding='utf-8') as f:
        json.dump({'wbdata': {k: sorted(v) for k, v in wb.items()},
                   'core13_mod': {k: sorted(v) for k, v in mo.items()}}, f, ensure_ascii=False, indent=1)

    # ---------------- 统计 ----------------
    by = collections.defaultdict(list)
    for x in rows:
        by[x['code_dec']].append(x['name'])
    coll = {k: v for k, v in by.items() if len(v) > 1}
    kinds = collections.Counter(x['kind'] for x in rows)
    print(f'[stat]  total names     = {len(rows)}')
    print(f'        with 中文名      = {sum(1 for x in rows if x["chinese"])}')
    print(f'        official 库命中   = {sum(1 for x in rows if x["in_official"] == "y")}')
    print(f'        summonable=yes   = {sum(1 for x in rows if x["summonable"] == "yes")}')
    print(f'        summonable=no    = {sum(1 for x in rows if x["summonable"] == "no")}')
    print(f'        hash collisions  = {len(coll)}')
    for k, v in kinds.most_common():
        print(f'          {KIND_CN.get(k, k):<20} {v}')
    print(f'[stats] trainer units   = {len(units)}（基准 {sum(1 for x in units if x["primary"] == "1")}）')
    cb = collections.Counter((x['faction'], x['unit_class']) for x in units)
    for (fa, cl), v in sorted(cb.items(), key=lambda kv: (FAC_ORDER.get(kv[0][0], 9),
                                                          CLASS_ORDER.get(kv[0][1], 9))):
        print(f'          {fa:<4} {cl:<5} {v}')
    print(f'[out]   {tsv}')
    print(f'[out]   {t_tsv}')
    print(f'[out]   {t_h}')


if __name__ == '__main__':
    main()
