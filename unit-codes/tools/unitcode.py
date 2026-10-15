"""unitcode.py — RA3 对象模板名 <-> 单位代码(哈希) 小工具

用法:
  python unitcode.py 名称 [名称...]        # 名称 -> 代码
  python unitcode.py --hex 0x509BD329      # 代码 -> 名称(查表)
  python unitcode.py --find 关键字          # 在总表里模糊搜索
  python unitcode.py --find 关键字 --primary   # 只看基准形态
  python unitcode.py --trainer             # 只搜修改器用的可玩对象表
"""
import sys, os, json, re

HERE = os.path.dirname(os.path.abspath(__file__))
ARCHIVE = os.path.abspath(os.path.join(HERE, '..'))
sys.path.insert(0, HERE)
from sage_hash import sage_hash

ALL_TABLE = os.path.join(ARCHIVE, 'data', 'unit_codes_all.tsv')
TRAINER_TABLE = os.path.join(ARCHIVE, 'data', 'unit_codes_trainer.tsv')


def load(path=None):
    rows = []
    p = path or ALL_TABLE
    if not os.path.exists(p):
        return rows
    with open(p, encoding='utf-8') as f:
        head = next(f, None)
        cols = head.rstrip('\n').split('\t') if head else []
        for line in f:
            v = line.rstrip('\n').split('\t')
            if len(v) >= 6:
                rows.append(dict(zip(cols, v)))
    return rows


def main():
    a = sys.argv[1:]
    if not a:
        print(__doc__)
        return
    if a[0] == '--hex':
        want = int(a[1], 16) & 0xFFFFFFFF
        for r in load():
            if int(r['code_dec']) == want:
                print(f"{r['code_hex']}\t{r['name']}\t{r.get('faction','')}\t{r.get('chinese_name','')}")
        return
    if a[0] == '--find':
        kw = a[1].lower()
        only_primary = '--primary' in a
        only_usable = '--usable' in a
        for r in load():
            if only_primary and r.get('is_primary') != '1':
                continue
            if only_usable and r.get('summonable') != 'yes':
                continue
            hay = (r['name'] + ' ' + r.get('chinese_name', '') + ' ' + r.get('family', '')).lower()
            if kw in hay:
                print(f"{r['code_hex']}\t{r['name']}\t{r.get('faction','')}\t{r.get('chinese_name','')}"
                      f"\t{r.get('unit_class','')}\tusable={r.get('summonable','?')}\tprimary={r.get('is_primary','?')}")
        return
    if a[0] == '--trainer':
        units = load(TRAINER_TABLE)
        print(f'trainer 表 {len(units)} 条（基准形态 {sum(1 for r in units if r.get("is_primary") == "1")} 条）')
        return
    for name in a:
        print(f'{name}\t0x{sage_hash(name.lower()):08X}')


if __name__ == '__main__':
    main()
