"""从 .lyi 解包目录生成协议/技能目录头文件。

数据来源（只读，不碰游戏文件）：
    <data>/mod.imp, mod_l.imp, mod_m.imp    编译符号表（内部名最全）
    <data>/maps/**/*.xml|json               地图覆盖里出现的引用
    <data>/scripts/*.lua|*.xml             作者脚本里的引用

产出 src/protocols_corona.h：
    kProtoPowers[]  可以授予/触发的 SpecialPower（技能）
    kProtoTechs[]   玩家科技（解锁用）

阵营由名字里的厂牌词推导 —— 这是**约定**，不是实测；推不出来的一律归"通用"。
"""
import os
import re
import sys

FACTIONS = [
    ("Celestial", "神州"),
    ("Soviet", "苏联"),
    ("Allied", "盟军"),
    ("Japan", "帝国"),
    ("Generic", "通用"),
]

# 名字里没有厂牌词的协议：按模组自带 SpecialPowerEnum.lua 的分组与字符串表键定点覆盖。
# （通用池里混着四个阵营的原版协议，不点名就只能标"通用"。）
FACTION_OVERRIDES = {
    "SpecialPowerParticleCannon": "盟军",          # 盟军超武 AlliedSuperWeaponAdvanced
    "SpecialPowerSatelliteSweep": "盟军",          # SpecialPowerEnum 盟军段
    "SpecialPowerHighTechnology": "盟军",          # SpecialPowerEnum 盟军段
    "SpecialPowerPointDefenseDrones": "帝国",      # SpecialPowerEnum 帝国段
    "SpecialPower_ProductionKickbacks": "苏联",    # SpecialPowerEnum 苏联段
    "SpecialPowerPause01": "神州",                 # 字符串表键 CelestialPlayerPowerPause
    "SpecialPower_GrantCelestialTech": "神州",
}

# 两种命名都存在：SpecialPower_Xxx（带下划线）与 SpecialPowerXxx（不带）。
# 后者会顺带匹配到 "SpecialPowerTemplate" 这个类型名，扫完用 _skip 过滤掉。
POWER_RE = re.compile(rb"SpecialPower_?([A-Z][A-Za-z0-9_]{3,})")
POWER_SKIP = ("Template", "Store")
TECH_RE = re.compile(rb"PlayerTech_([A-Z][A-Za-z0-9_]{3,})")

# ---- 授予门槛（2026-10-19 两局实测，判据是读引擎容器，不是看脚本返回值）----
# PLAYER_GRANT_SPECIAL_POWER 要求模板的 [[tpl+8]+0x134] 非空，该字段是「玩家协议记录」指针；
# 为 0 时引擎在循环里 continue，列表不增长、也不报错 —— 面板上点它只会没反应。
# 下列模板在本模组里该字段为 0。测量方法见 tools/proto_probe.py（只读，进对局后跑）。
NO_PLAYER_POWER = {
    "SpecialPowerCelestialCannon",                 # 天朝主炮（舰炮类单位技能）
    "SpecialPower_CelestialMassiveOrbitalStrike",  # 大规模轨道打击
    "SpecialPower_CelestialPowerPlantDetection",   # 弧光破云
    "SpecialPowerDragonShipAttackDrones",          # 蜂群出击
    "SpecialPowerChronosphereObjectSelect",        # 超时空传送仪
    "SpecialPowerAlliedMissileCrisis",             # 战术打击
    "SpecialPowerAlliedRangerAirAssault",          # 突击行动
    "SpecialPowerAlliedShipboardWarplaneSquadron", # 区域轰炸
    "SpecialPowerAlliedPrecisionAssault",          # 黑鸟行动
    "SpecialPowerIronCurtain",                     # 铁幕
    "SpecialPowerVacuumBomb",                      # 真空内爆弹
    "SpecialPowerNanoSwarmHive",                   # 纳米虫群
    "SpecialPowerTelekineticProjectorDevice",      # 超能波毁灭装置
    "SpecialPowerPause01",                         # 止戈力场
    "SpecialPowerParticleCannon",                  # 质子撞击炮
}

# +0x134 与另一条指向同一个协议记录 ⇒ 0x854300 按指针去重，后授予的那个列表不增长。
SHARED_OWNER = {
    "SpecialPower_CelestialPantaOrbitalStrike": "SpecialPower_CelestialOrbitalStrike",
}

KIND_OK, KIND_NO_POWER, KIND_SHARED = 0, 1, 2


def pick_kind(name):
    if name in NO_PLAYER_POWER:
        return KIND_NO_POWER
    if name in SHARED_OWNER:
        return KIND_SHARED
    return KIND_OK



def scan(root):
    powers, techs = set(), set()
    for base, _, files in os.walk(root):
        for fn in files:
            ext = os.path.splitext(fn)[1].lower()
            if ext not in (".imp", ".xml", ".json", ".lua", ".map", ".manifest", ".str"):
                continue
            p = os.path.join(base, fn)
            try:
                if os.path.getsize(p) > 64 << 20:
                    continue
                with open(p, "rb") as f:
                    blob = f.read()
            except OSError:
                continue
            for m in POWER_RE.finditer(blob):
                nm = m.group(1).decode("latin1")
                if nm.startswith(POWER_SKIP):
                    continue
                powers.add("SpecialPower" + ("_" if m.group(0).startswith(b"SpecialPower_") else "") + nm)
            for m in TECH_RE.finditer(blob):
                techs.add("PlayerTech_" + m.group(1).decode("latin1"))
    return sorted(powers), sorted(techs)


def faction_of(name):
    if name in FACTION_OVERRIDES:
        return FACTION_OVERRIDES[name]
    for token, cn in FACTIONS:
        if token.lower() in name.lower():
            return cn
    return "通用"


def short(name):
    for pre in ("SpecialPower_", "PlayerTech_"):
        if name.startswith(pre):
            return name[len(pre):]
    return name


def load_picks(path):
    """读 proto_zh.tsv（内部名 / 官方中文名 / 阵营 / 官方描述）。

    TSV 由 tools/extract_official.py 从日冕 gamestrings.csf 生成，别手改。
    """
    out = []
    if not os.path.isfile(path):
        return out
    with open(path, encoding="utf-8") as f:
        for ln in f:
            ln = ln.rstrip("\r\n")
            if not ln or ln.startswith("#"):
                continue
            parts = ln.split("\t")
            if len(parts) < 3:
                continue
            desc = parts[3].strip() if len(parts) > 3 else ""
            out.append((parts[0].strip(), parts[1].strip(), parts[2].strip(), desc))
    return out


def cstr(s):
    return '"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'


def emit(path, powers, techs, picks):
    L = []
    L.append("// 由 tools/gen_protocols.py 从 .lyi 解包数据自动生成，请勿手改。")
    L.append("// 数据源：mod.imp / maps/**/overrides.xml / scripts/**（只读解包产物）")
    L.append("#pragma once")
    L.append("")
    L.append("namespace ra3 {")
    L.append("")
    L.append("struct ProtoEntry { const char* id; const char* name; const char* faction; };")
    L.append("")
    L.append("static const ProtoEntry kProtoPowers[] = {")
    for p in powers:
        L.append("    { %s, %s, %s }," % (cstr(p), cstr(short(p)), cstr(faction_of(p))))
    L.append("};")
    L.append("static const int kProtoPowerCount = %d;" % len(powers))
    L.append("")
    L.append("static const ProtoEntry kProtoTechs[] = {")
    for t in techs:
        L.append("    { %s, %s, %s }," % (cstr(t), cstr(short(t)), cstr(faction_of(t))))
    L.append("};")
    L.append("static const int kProtoTechCount = %d;" % len(techs))
    L.append("")
    L.append("// ---- 协议技能（人工整理中文名，见 tools/proto_zh.tsv）----")
    L.append("// kind：0=可授予；1=本模组无玩家协议记录（引擎静默跳过）；2=与另一条共用同一协议记录（先去重）。")
    L.append("// 1/2 两类由实测得出，判定依据见 tools/proto_probe.py 与本文件 NO_PLAYER_POWER / SHARED_OWNER。")
    L.append("struct ProtoPick { const char* id; const char* zh; const char* faction; const char* desc; int kind; };")
    L.append("")
    L.append("static const ProtoPick kProtoPicks[] = {")
    for pid, zh, fac, desc in picks:
        L.append("    { %s, %s, %s, %s, %d }," % (cstr(pid), cstr(zh), cstr(fac), cstr(desc),
                                                 pick_kind(pid)))
    L.append("};")
    L.append("static const int kProtoPickCount = %d;" % len(picks))
    n_nopower = sum(1 for p in picks if pick_kind(p[0]) == KIND_NO_POWER)
    n_shared = sum(1 for p in picks if pick_kind(p[0]) == KIND_SHARED)
    L.append("// 其中无玩家协议记录 %d 条、共用记录 %d 条。" % (n_nopower, n_shared))
    L.append("")
    L.append("} // namespace ra3")
    L.append("")
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(L))


def main(argv):
    root = argv[0] if argv else r"C:\Project\RA3\research\lyi\out\corona_3.258\data"
    out = argv[1] if len(argv) > 1 else os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "src", "protocols_corona.h")
    if not os.path.isdir(root):
        print("解包目录不存在: %s" % root)
        return 1
    powers, techs = scan(root)
    tsv = os.path.join(os.path.dirname(os.path.abspath(__file__)), "proto_zh.tsv")
    picks = load_picks(tsv)
    emit(out, powers, techs, picks)
    from collections import Counter
    print("powers=%d  techs=%d  -> %s" % (len(powers), len(techs), out))
    print("powers 阵营:", Counter(faction_of(p) for p in powers).most_common())
    print("techs  阵营:", Counter(faction_of(t) for t in techs).most_common())
    print("picks 中文名: %d 条（含官方描述 %d 条）"
          % (len(picks), sum(1 for p in picks if p[3])))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
