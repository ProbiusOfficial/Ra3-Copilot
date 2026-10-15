#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""cdp_overflow.py - 用 CDP 在指定视口宽度下找横向溢出元素

用法: python cdp_overflow.py <url> <width> [height]
输出: 视口内 scrollWidth / clientWidth 与所有越界元素（按越界量排序）
"""
import json
import subprocess
import sys
import time
import urllib.request

import websocket

EDGE_CANDIDATES = [
    r'C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe',
    r'C:\Program Files\Microsoft\Edge\Application\msedge.exe',
]

JS = r"""
(() => {
  const w = document.documentElement.clientWidth;
  const rows = [];
  document.querySelectorAll('*').forEach(el => {
    const r = el.getBoundingClientRect();
    if (r.width === 0 && r.height === 0) return;
    const over = Math.round(Math.max(r.right - w, -r.left));
    if (over > 1) {
      const cls = (typeof el.className === 'string' ? el.className : '');
      rows.push({ sel: el.tagName.toLowerCase() + (cls ? '.' + cls.trim().split(/\s+/).join('.') : ''),
                  left: Math.round(r.left), right: Math.round(r.right), over: over });
    }
  });
  rows.sort((a, b) => b.over - a.over);
  return JSON.stringify({
    docScrollWidth: document.documentElement.scrollWidth,
    clientWidth: w,
    bodyScrollWidth: document.body.scrollWidth,
    rows: rows.slice(0, 25)
  });
})()
"""


def main():
    url = sys.argv[1]
    vw = int(sys.argv[2]) if len(sys.argv) > 2 else 430
    vh = int(sys.argv[3]) if len(sys.argv) > 3 else 900

    exe = next((p for p in EDGE_CANDIDATES if __import__('os').path.exists(p)), None)
    if not exe:
        print('edge not found')
        return
    port = 9333
    proc = subprocess.Popen([exe, '--headless=new', '--disable-gpu', f'--remote-debugging-port={port}',
                             '--remote-allow-origins=*',
                             '--user-data-dir=' + __import__('tempfile').mkdtemp(), 'about:blank'],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(40):
            try:
                targets = json.load(urllib.request.urlopen(f'http://127.0.0.1:{port}/json'))
                if targets:
                    break
            except Exception:
                time.sleep(0.25)
        ws_url = next(t['webSocketDebuggerUrl'] for t in targets if t['type'] == 'page')
        ws = websocket.create_connection(ws_url, timeout=20)
        i = [0]

        def cmd(method, params=None):
            i[0] += 1
            ws.send(json.dumps({'id': i[0], 'method': method, 'params': params or {}}))
            while True:
                msg = json.loads(ws.recv())
                if msg.get('id') == i[0]:
                    return msg

        cmd('Page.enable')
        cmd('Emulation.setDeviceMetricsOverride',
            {'width': vw, 'height': vh, 'deviceScaleFactor': 1, 'mobile': True})
        cmd('Page.navigate', {'url': url})
        time.sleep(2.5)
        res = cmd('Runtime.evaluate', {'expression': JS, 'returnByValue': True})
        print(json.dumps(json.loads(res['result']['result']['value']), ensure_ascii=False, indent=1))
    finally:
        proc.terminate()


if __name__ == '__main__':
    main()
