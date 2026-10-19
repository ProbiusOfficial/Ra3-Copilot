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

# 两种命名都存在：SpecialPower_Xxx（带下划线）与 SpecialPowerXxx（不带）。
# 后者会顺带匹配到 "SpecialPowerTemplate" 这个类型名，扫完用 _skip 过滤掉。
POWER_RE = re.compile(rb"SpecialPower_?([A-Z][A-Za-z0-9_]{3,})")
POWER_SKIP = ("Template", "Store")
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
                nm = m.group(1).decode("latin1")
                if nm.startswith(POWER_SKIP):
                    continue
                powers.add("SpecialPower" + ("_" if m.group(0).startswith(b"SpecialPower_") else "") + nm)
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


def load_picks(path):
    """读人工维护的 proto_zh.tsv（内部名 / 中文名 / 阵营）。"""
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
            out.append((parts[0].strip(), parts[1].strip(), parts[2].strip()))
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
    L.append("// 只有这批是「面板上能选、能授予」的协议；其余 SpecialPower 多为单位能力，引擎会拒绝。")
    L.append("struct ProtoPick { const char* id; const char* zh; const char* faction; };")
    L.append("")
    L.append("static const ProtoPick kProtoPicks[] = {")
    for pid, zh, fac in picks:
        L.append("    { %s, %s, %s }," % (cstr(pid), cstr(zh), cstr(fac)))
    L.append("};")
    L.append("static const int kProtoPickCount = %d;" % len(picks))
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
    print("picks 中文名: %d 条" % len(picks))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
