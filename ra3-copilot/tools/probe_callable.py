#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""probe_callable.py - 逐条实测「这个哈希在游戏里能不能找到模板」

原理：调用修改器的 HTTP 接口 /resolve?hash=0xXXXX，它内部走的就是召唤用的
      SymTmplFind(0x3E4230) -> ThingFactory。返回 template 00000000 即 = 召唤必失败。

产出：
  unit-codes/data/callable_probe.tsv  逐条结果（faction/chinese/id/code/ok）
  控制台统计：总数 / 可召唤 / 不可召唤，并按阵营、按来源拆分

用法：python probe_callable.py [--tsv path]
"""
import csv
import io
import os
import sys
import urllib.request
from collections import Counter

HERE = os.path.dirname(os.path.abspath(__file__))
WS = os.path.dirname(os.path.dirname(HERE))          # .../RA3
TSV = os.path.join(WS, 'unit-codes', 'data', 'unit_codes_trainer.tsv')
OUT = os.path.join(WS, 'unit-codes', 'data', 'callable_probe.tsv')
API = 'http://127.0.0.1:17832/resolve?hash=%s'


def resolve(code_hex):
    try:
        with urllib.request.urlopen(API % code_hex, timeout=4) as f:
            txt = f.read().decode('utf-8', 'replace').strip()
    except Exception as e:
        return None, 'ERR:%s' % e
    found = 'template 00000000' not in txt
    return found, txt


def main():
    if len(sys.argv) > 2 and sys.argv[1] == '--tsv':
        tsv = sys.argv[2]
    else:
        tsv = TSV
    rows = list(csv.DictReader(io.open(tsv, encoding='utf-8'), delimiter='\t'))
    print('[*] %d 条，开始逐条探测 %s' % (len(rows), API))

    results = []
    ok_by_fac = Counter()
    all_by_fac = Counter()
    for i, r in enumerate(rows):
        code = r['code_hex'].strip()
        found, raw = resolve(code)
        results.append((r['faction'], r['chinese_name'], r['name'], code, found))
        all_by_fac[r['faction']] += 1
        if found:
            ok_by_fac[r['faction']] += 1
        if (i + 1) % 100 == 0:
            print('   ... %d/%d' % (i + 1, len(rows)))

    n_ok = sum(1 for x in results if x[4])
    with io.open(OUT, 'w', encoding='utf-8', newline='\n') as f:
        f.write('faction\tchinese_name\tid\tcode_hex\tcallable\n')
        for fac, cn, uid, code, found in results:
            f.write('%s\t%s\t%s\t%s\t%d\n' % (fac, cn, uid, code, 1 if found else 0))

    print('\n===== 结果 =====')
    print('总数 %d  可召唤 %d  不可召唤 %d' % (len(results), n_ok, len(results) - n_ok))
    print('\n按阵营：')
    for fac in sorted(all_by_fac):
        print('  %-6s %3d/%3d  （失败 %d）' % (fac, ok_by_fac[fac], all_by_fac[fac],
                                              all_by_fac[fac] - ok_by_fac[fac]))
    print('\n失败样例（前 25 条）：')
    bad = [x for x in results if not x[4]]
    for fac, cn, uid, code, _ in bad[:25]:
        print('  %-4s %-28s %-42s %s' % (fac, cn[:28], uid[:42], code))
    print('\n[ok] 明细写入 %s' % OUT)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
