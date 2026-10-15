#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""gen_mcp_catalog.py - 为 MCP 服务器生成单位目录 JSON

数据源：
  <ws>/unit-codes/data/unit_codes_all.tsv    全量 5461（筛「可玩对象」∪「可召唤」）
  ra3-copilot/src/units_corona.h 的 kCrates[]  54 个箱子（/crate 的 idx 顺序 == 这里的下标）
产出：
  ra3-copilot/mcp/unit_catalog.json  （UTF-8；MCP 服务器启动时读；缺失则退化为"只能按哈希"）
"""
import csv, io, json, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
WS = os.path.dirname(ROOT)
SRC = os.path.join(WS, 'unit-codes', 'data', 'unit_codes_all.tsv')
UNITS_H = os.path.join(ROOT, 'src', 'units_corona.h')
OUTDIR = os.path.join(ROOT, 'mcp')
OUT = os.path.join(OUTDIR, 'unit_catalog.json')

PLAYABLE = '可玩对象(单位/建筑)'

try:
    from pypinyin import lazy_pinyin, Style
    def py_of(s):
        if not s or not any('\u4e00' <= c <= '\u9fff' for c in s):
            return ''
        f = ''.join(lazy_pinyin(s, style=Style.FIRST_LETTER))
        q = ''.join(lazy_pinyin(s))
        return f + ' ' + q
except Exception:
    def py_of(s):
        return ''

# 复用 units_corona.h 的箱子行（哈希已验证）
CRATE_RE = re.compile(r'u8"([^"]*)"\s*,\s*u8"[^"]*"\s*,\s*u8"([^"]*)"\s*,\s*"([^"]*)"\s*,\s*"[^"]*"\s*,\s*(0x[0-9A-Fa-f]{8})u')


def main():
    rows = list(csv.DictReader(io.open(SRC, encoding='utf-8'), delimiter='\t'))
    units = []
    seen = set()
    for r in rows:
        kind = (r.get('kind_cn') or '').strip()
        summ = (r.get('summonable') or '').strip().lower() == 'yes'
        if kind != PLAYABLE and not summ:
            continue
        h = (r.get('code_hex') or '').strip()
        try:
            hv = int(h, 16)
        except ValueError:
            continue
        if hv in seen:
            continue
        seen.add(hv)
        name = (r.get('chinese_name') or '').strip() or (r.get('name') or '').strip()
        units.append({
            'n': name,
            'i': (r.get('name') or '').strip(),
            'h': '0x%08X' % hv,
            's': (r.get('faction') or '').strip(),
            'c': (r.get('unit_class') or '').strip() or (r.get('db_category') or '').strip(),
            'k': kind,
            'm': 1 if summ else 0,
            'p': py_of(name),
        })

    crates = []
    if os.path.exists(UNITS_H):
        txt = io.open(UNITS_H, encoding='utf-8').read()
        m = re.search(r'kCrates\[\]\s*=\s*\{(.*?)\n\};', txt, re.S)
        if m:
            for fac, cn, uid, code in CRATE_RE.findall(m.group(1)):
                crates.append({'n': cn, 'i': uid, 'h': code.upper().replace('0X', '0x'),
                               's': fac, 'c': '箱子', 'k': '可玩对象(单位/建筑)', 'm': 1, 'p': py_of(cn)})

    if not os.path.isdir(OUTDIR):
        os.makedirs(OUTDIR)
    doc = {
        '_note': '自动生成（ra3-copilot/tools/gen_mcp_catalog.py）。n=显示名 i=模板名 h=类型哈希 s=阵营 c=类别 k=原类型 m=可召唤 p=拼音',
        'units': units,
        'crates': crates,
    }
    io.open(OUT, 'w', encoding='utf-8', newline='\n').write(
        json.dumps(doc, ensure_ascii=False, separators=(',', ':')))
    print('[ok] %s  单位 %d 条 / 箱子 %d 条 / %.0f KB'
          % (OUT, len(units), len(crates), os.path.getsize(OUT) / 1024.0))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
