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

POWER_RE = re.compile(rb"SpecialPower_([A-Z][A-Za-z0-9_]{3,})")
TECH_RE = re.compile(rb"PlayerTech_([A-Z][A-Za-z0-9_]{3,})")


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
                powers.add("SpecialPower_" + m.group(1).decode("latin1"))
            for m in TECH_RE.finditer(blob):
                techs.add("PlayerTech_" + m.group(1).decode("latin1"))
    return sorted(powers), sorted(techs)


def faction_of(name):
    for token, cn in FACTIONS:
        if token.lower() in name.lower():
            return cn
    return "通用"


def short(name):
    for pre in ("SpecialPower_", "PlayerTech_"):
        if name.startswith(pre):
            return name[len(pre):]
    return name


def cstr(s):
    return '"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'


def emit(path, powers, techs):
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
    emit(out, powers, techs)
    from collections import Counter
    print("powers=%d  techs=%d  -> %s" % (len(powers), len(techs), out))
    print("powers 阵营:", Counter(faction_of(p) for p in powers).most_common())
    print("techs  阵营:", Counter(faction_of(t) for t in techs).most_common())
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
