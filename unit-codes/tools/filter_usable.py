#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""filter_usable.py — 给「能召唤」的清单再补上一刀「召唤出来能不能用」

背景
  实测（`ThingFactory::Find` / HTTP `/resolve`）只能证明**模板存在、能创建**，
  证明不了**创建出来能不能操作**。反例（用户实测）：
      波塞冬巡洋舰  0x9356389F  AlliedAntiNavyShipTech3        ← 能正常用  ✅
                    0xDB5A5941  AlliedChronoBeacon_Poseidon    ← 召唤出来不可选/不能用 ❌
  两个模板在运行时都能 resolve，光靠实测分不出来。

判据（来自官方单位图鉴数据 kind_of 位掩码）
  `SELECTABLE` ∈ kind_of  →  玩家能选中、能操作  =  可用
  缺 `SELECTABLE` 的典型：
    AlliedChronoBeacon_*   超时空传送抵达体（IGNORES_SELECT_ALL）
    CelestialAirDrop_Land  空投运输体（TRANSPORT）
    SovietTeslaAirAssault  技能召唤的运输飞行器（SUMMONED + IGNORES_SELECT_ALL）
    AlliedPowerPlantTurbine 建筑子装置（UNATTACKABLE + SUMMONED）

产出
  unit-codes/data/callable_final.tsv    ← 就地过滤（原文件备份为 .bak_<时间戳>）
  unit-codes/data/callable_dropped.tsv  ← 被剔除的条目 + 原因（留档）

用法
  python filter_usable.py            # 需要先有 callable_final.tsv
"""
import csv
import io
import os
import shutil
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ARCHIVE = os.path.abspath(os.path.join(HERE, '..'))
WS = os.path.abspath(os.path.join(ARCHIVE, '..'))
sys.path.insert(0, HERE)
from official_db import is_usable, reason_unusable, record  # noqa: E402

D = os.path.join(ARCHIVE, 'data')
FINAL = os.path.join(D, 'callable_final.tsv')
DROPPED = os.path.join(D, 'callable_dropped.tsv')

# 不在官方库时，按名字可判定的坏形态
BAD_FAMILY = ('ChronoBeacon',)


def main():
    if not os.path.exists(FINAL):
        print('[x] 找不到 %s' % FINAL)
        return 1

    rows = list(csv.DictReader(io.open(FINAL, encoding='utf-8'), delimiter='\t'))
    print('[*] 读入 %s：%d 条' % (os.path.basename(FINAL), len(rows)))

    keep, drop = [], []
    for r in rows:
        uid = (r.get('id') or '').strip()
        verdict = is_usable(uid)
        if verdict is True:
            keep.append(r)
            continue
        if verdict is False:
            why = reason_unusable(uid)
        else:
            rec = record(uid)                      # None → 不在官方库
            if any(b in uid for b in BAD_FAMILY):
                why = '名字属于 %s（传送/空投抵达体）' % '/'.join(BAD_FAMILY)
            else:
                keep.append(r)                     # 无法判定 → 保守保留
                r['note'] = '不在官方图鉴库，未能核验可用性'
                continue
        r['reason'] = why
        drop.append(r)

    if not drop:
        print('[=] 没有需要剔除的条目')
        return 0

    bak = FINAL + '.bak_' + time.strftime('%Y%m%d_%H%M%S')
    shutil.copy2(FINAL, bak)

    fields = list(rows[0].keys())
    with io.open(FINAL, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\t'.join(fields) + '\n')
        for r in keep:
            f.write('\t'.join((r.get(k) or '') for k in fields) + '\n')

    with io.open(DROPPED, 'w', encoding='utf-8', newline='\n') as f:
        f.write('faction\tchinese_name\tid\tcode_hex\treason\n')
        for r in drop:
            f.write('%s\t%s\t%s\t%s\t%s\n' % (r.get('faction', ''), r.get('chinese_name', ''),
                                              r.get('id', ''), r.get('code_hex', ''), r['reason']))

    print('[*] 备份 → %s' % os.path.basename(bak))
    print('[*] 保留 %d 条，剔除 %d 条' % (len(keep), len(drop)))
    print('\n剔除明细：')
    for r in drop:
        print('  %-6s %-16s %-42s %s' % (r.get('faction', ''), r.get('chinese_name', ''),
                                         r.get('id', ''), r['reason']))
    print('\n[ok] 写入 %s' % FINAL)
    print('[ok] 写入 %s' % DROPPED)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
