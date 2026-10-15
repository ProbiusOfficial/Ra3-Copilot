#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""gen_battle_catalog.py - 由 unit-codes 全量表生成 src/battle_catalog.h

用途：给「战场枚举」（MCP /snapshot）提供 ①可玩对象过滤集 ②哈希→名字/阵营/类型。
数据源：<ws>/unit-codes/data/unit_codes_all.tsv（全量 5461 条）
筛选   ：kind_cn == '可玩对象(单位/建筑)'  →  约 938 条
产出   ：src/battle_catalog.h（UTF-8 无 BOM；编译带 /utf-8；按 hash 升序便于二分）
"""
import csv, io, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
WS = os.path.dirname(ROOT)
SRC = os.path.join(WS, 'unit-codes', 'data', 'unit_codes_all.tsv')
OUT = os.path.join(ROOT, 'src', 'battle_catalog.h')

PLAYABLE = '可玩对象(单位/建筑)'

SIDE_ORDER = ['神州', '盟军', '苏联', '帝国', '中立', '平民', '特殊']
CLS_ORDER = ['其他', '步兵', '载具', '空军', '海军', '建筑', '箱子']


def esc(s):
    return (s or '').replace('\\', '\\\\').replace('"', '\\"')


def main():
    rows = list(csv.DictReader(io.open(SRC, encoding='utf-8'), delimiter='\t'))
    play = [r for r in rows if (r.get('kind_cn') or '').strip() == PLAYABLE]
    out = []
    for r in play:
        try:
            h = int((r.get('code_hex') or '').strip(), 16)
        except ValueError:
            continue
        name = (r.get('chinese_name') or '').strip() or (r.get('name') or '').strip()
        cat = (r.get('db_category') or '').strip()
        cat_i = 0 if cat == 'unit' else (1 if cat == 'structure' else 2)
        cls = (r.get('unit_class') or '').strip()
        cls_i = CLS_ORDER.index(cls) if cls in CLS_ORDER else 0
        side = (r.get('faction') or '').strip()
        side_i = SIDE_ORDER.index(side) if side in SIDE_ORDER else 0
        out.append((h, name, (r.get('name') or '').strip(), side_i, cls_i, cat_i))

    # 去重（同一 hash 只留第一条）
    seen, uniq = set(), []
    for e in sorted(out, key=lambda x: x[0]):
        if e[0] in seen:
            continue
        seen.add(e[0]); uniq.append(e)

    lines = []
    for h, name, tid, side_i, cls_i, cat_i in uniq:
        lines.append('    { 0x%08Xu, u8"%s", "%s", %d, %d, %d },'
                     % (h, esc(name), esc(tid), side_i, cls_i, cat_i))

    head = '''// battle_catalog.h - 战场对象目录（自动生成，请勿手改）
// 生成器 : ra3-copilot/tools/gen_battle_catalog.py
// 数据源 : unit-codes/data/unit_codes_all.tsv（全量 5461）中 kind_cn='可玩对象(单位/建筑)'
// 用途   : 「战场枚举」时按哈希→名字/阵营/类型；同时用成员资格过滤掉矿晶/树/弹道等场景物
// 说明   : 哈希 = StringHasher.Hash(模板名.lower())；按 hash 升序，二分查找
#pragma once
#include <cstdint>

namespace ra3 {

// 阵营枚举（与 kBattleSideNames 对齐）
enum { B_SIDE_CELESTIAL = 0, B_SIDE_ALLIED, B_SIDE_SOVIET, B_SIDE_JAPAN,
       B_SIDE_NEUTRAL, B_SIDE_CIVILIAN, B_SIDE_SPECIAL, B_SIDE_COUNT };

// 类型枚举（与 kBattleClassNames 对齐）
enum { B_CLS_OTHER = 0, B_CLS_INFANTRY, B_CLS_VEHICLE, B_CLS_AIR,
       B_CLS_NAVAL, B_CLS_STRUCTURE, B_CLS_CRATE, B_CLS_COUNT };

struct BattleObj {
    uint32_t    hash;   // 对象类型哈希
    const char* name;   // 显示名（中文优先，退回英文名）
    const char* id;     // 对象模板名（供召唤）
    uint8_t     side;   // B_SIDE_*
    uint8_t     cls;    // B_CLS_*
    uint8_t     cat;    // 0=unit 1=structure 2=other
};

static const char* const kBattleSideNames[]   = { u8"神州", u8"盟军", u8"苏联", u8"帝国", u8"中立", u8"平民", u8"特殊" };
static const char* const kBattleClassNames[]  = { u8"其他", u8"步兵", u8"载具", u8"空军", u8"海军", u8"建筑", u8"箱子" };

// 按 hash 升序（二分查找依赖此序）
static const BattleObj kBattleObjects[] = {
%s
};
static const int kBattleObjectCount = %d;

// hash -> 目录项；找不到返回 nullptr
inline const BattleObj* BattleLookup(uint32_t h) {
    int lo = 0, hi = kBattleObjectCount - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        if (kBattleObjects[mid].hash == h) return &kBattleObjects[mid];
        if (kBattleObjects[mid].hash < h) lo = mid + 1; else hi = mid - 1;
    }
    return nullptr;
}

} // namespace ra3
''' % ('\n'.join(lines), len(uniq))

    io.open(OUT, 'w', encoding='utf-8', newline='\n').write(head)
    print('[ok] 写出 %s：%d 条' % (OUT, len(uniq)))
    import collections
    print('     阵营:', collections.Counter(SIDE_ORDER[e[3]] for e in uniq).most_common())
    print('     类型:', collections.Counter(CLS_ORDER[e[4]] for e in uniq).most_common())
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
