#!/usr/bin/env python3
"""gen_cs_pairs.py - 从配对数推导结果生成 cs_pairs.h（CommandSet key 映射表）

数据源：.workbuddy/tmp/lcs/pairs.json
  每条 = {base, base_key, enh, enh_key, n_base, n_enh, delta, building}
  由 final_pairs.py 从 mod.imp 的 LogicCommandSet 目录项自动推导。

输出：ra3-copilot/src/cs_pairs.h
  只收 building=true 的（建造页构建器只服务建筑；单位指令栏不走 0x6CB330）。
  同一 base_key 出现多次（日冕有 Transition → Tech2 两个分支）时保留条目数最多的那条，
  因为映射只能选一个；被丢弃的记在注释里，便于以后按场景细分。
"""
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(ROOT, '.workbuddy', 'tmp', 'lcs', 'pairs.json')
OUT = os.path.join(ROOT, 'ra3-copilot', 'src', 'cs_pairs.h')


def main():
    pairs = json.load(open(SRC, encoding='utf-8'))
    bld = [p for p in pairs if p.get('building')]
    best = {}
    dropped = []
    for p in bld:
        k = int(p['base_key'], 16)
        enh = int(p['enh_key'], 16)
        if enh == k:
            continue
        n = p.get('n_enh') or 0
        if k in best:
            if n > (best[k][1].get('n_enh') or 0):
                dropped.append(best[k][1])
                best[k] = (enh, p)
            else:
                dropped.append(p)
        else:
            best[k] = (enh, p)

    rows = sorted(best.items())
    lines = []
    lines.append('// cs_pairs.h - CommandSet key 映射表（自动生成，请勿手改）')
    lines.append('// 生成器 : ra3-copilot/tools/gen_cs_pairs.py')
    lines.append('// 数据源 : mod.imp 的 LogicCommandSet 目录项（base_key -> enh_key）')
    lines.append('// 用途   : 0x1F「扩展模式列表」在 0x6CB344 把建造页的 cs key 换成同名 Enhanced 版')
    lines.append('#pragma once')
    lines.append('#include <cstdint>')
    lines.append('')
    lines.append('namespace ra3 {')
    lines.append('')
    lines.append('struct CsPair { uint32_t base; uint32_t enh; };')
    lines.append('')
    lines.append('static const CsPair kCsPairs[] = {')
    for k, (enh, p) in rows:
        note = p['base']
        lines.append('    { 0x%08Xu, 0x%08Xu },   // %s  %s->%s' % (
            k, enh, note, p.get('n_base'), p.get('n_enh')))
    lines.append('};')
    lines.append('static const int kCsPairCount = %d;' % len(rows))
    lines.append('')
    lines.append('}  // namespace ra3')
    if dropped:
        lines.append('')
        lines.append('// 同 base_key 的其它分支（未采用，映射只能选一条）：')
        for p in dropped:
            lines.append('//   0x%s -> 0x%s  %s  %s->%s' % (
                p['base_key'], p['enh_key'], p['base'], p.get('n_base'), p.get('n_enh')))

    with open(OUT, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')
    print('wrote %s : %d pairs (%d dropped)' % (OUT, len(rows), len(dropped)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
