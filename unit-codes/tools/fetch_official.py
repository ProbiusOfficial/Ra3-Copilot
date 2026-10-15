#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""fetch_official.py — 重新拉取官方单位图鉴数据

来源：https://cor-games.com/units/  （日冕 Mod 官方网站「单位图鉴」）
      页面本身不含数据，由前端 fetch 这个文件：
      https://cor-games.com/data/game_objects/game_objects.jsonl

产出：../data/source/game_objects.jsonl

用法：python fetch_official.py [--proxy http://127.0.0.1:7890]
"""
import os
import sys
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(os.path.abspath(os.path.join(HERE, '..')), 'data', 'source', 'game_objects.jsonl')
URL = 'https://cor-games.com/data/game_objects/game_objects.jsonl'
UA = 'Mozilla/5.0 (Windows NT 10.0; Win64; x64)'


def main():
    proxy = None
    if '--proxy' in sys.argv:
        proxy = sys.argv[sys.argv.index('--proxy') + 1]
        os.environ['http_proxy'] = os.environ['https_proxy'] = proxy

    print('[*] 下载 %s' % URL)
    req = urllib.request.Request(URL, headers={'User-Agent': UA})
    t0 = time.time()
    data = urllib.request.urlopen(req, timeout=120).read()
    print('[*] %d 字节，用时 %.1fs' % (len(data), time.time() - t0))

    n = data.count(b'\n') + 1
    if not data.lstrip().startswith(b'{'):
        print('[x] 返回内容不像 jsonl，已放弃写入')
        return 1

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, 'wb') as f:
        f.write(data)
    print('[*] 约 %d 条 → %s' % (n, OUT))
    print('[ok] 拉取完成；接着跑 gen_unit_codes.py 重建全量表')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
