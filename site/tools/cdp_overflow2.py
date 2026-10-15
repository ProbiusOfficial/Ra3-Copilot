#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""cdp_overflow2.py - 列出真正导致页面横向溢出的元素（排除被 overflow 裁剪的子树）

用法: python cdp_overflow2.py <url> <width> [height]
"""
import json
import os
import subprocess
import sys
import tempfile
import time
import urllib.request

import websocket

EDGE = [r'C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe',
        r'C:\Program Files\Microsoft\Edge\Application\msedge.exe']

JS = r"""
(() => {
  const vw = document.documentElement.clientWidth;
  const clipped = el => {
    let p = el.parentElement;
    while (p && p !== document.body){
      const ov = getComputedStyle(p).overflowX;
      if (ov !== 'visible') return true;
      p = p.parentElement;
    }
    return false;
  };
  const rows = [];
  document.querySelectorAll('*').forEach(el => {
    const r = el.getBoundingClientRect();
    if (r.width < 3 || r.right <= vw + 1) return;
    if (clipped(el)) return;
    const cls = (typeof el.className === 'string') ? el.className.trim().replace(/\s+/g, '.') : '';
    rows.push({sel: el.tagName.toLowerCase() + (cls ? '.' + cls : ''),
               left: Math.round(r.left), right: Math.round(r.right), over: Math.round(r.right - vw)});
  });
  rows.sort((a, b) => b.over - a.over);
  return JSON.stringify({docScrollWidth: document.documentElement.scrollWidth, clientWidth: vw, rows: rows.slice(0, 20)});
})()
"""


def main():
    url, vw, vh = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]) if len(sys.argv) > 3 else 900
    exe = next((p for p in EDGE if os.path.exists(p)), None)
    port = 9350
    proc = subprocess.Popen([exe, '--headless=new', '--disable-gpu', f'--remote-debugging-port={port}',
                             '--remote-allow-origins=*', '--user-data-dir=' + tempfile.mkdtemp(), 'about:blank'],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        targets = None
        for _ in range(40):
            try:
                targets = json.load(urllib.request.urlopen(f'http://127.0.0.1:{port}/json'))
                if targets:
                    break
            except Exception:
                time.sleep(0.25)
        ws = websocket.create_connection(
            [t for t in targets if t['type'] == 'page'][0]['webSocketDebuggerUrl'], timeout=20)
        n = [0]

        def cmd(method, params=None):
            n[0] += 1
            ws.send(json.dumps({'id': n[0], 'method': method, 'params': params or {}}))
            while True:
                msg = json.loads(ws.recv())
                if msg.get('id') == n[0]:
                    return msg

        cmd('Page.enable')
        cmd('Emulation.setDeviceMetricsOverride', {'width': vw, 'height': vh, 'deviceScaleFactor': 1, 'mobile': True})
        cmd('Page.navigate', {'url': url})
        time.sleep(2.5)
        res = cmd('Runtime.evaluate', {'expression': JS, 'returnByValue': True})
        d = json.loads(res['result']['result']['value'])
        print(f"scrollWidth={d['docScrollWidth']} clientWidth={d['clientWidth']}")
        for r in d['rows']:
            print(f"  over={r['over']:>5}  {r['sel']}  [{r['left']}..{r['right']}]")
    finally:
        proc.terminate()


if __name__ == '__main__':
    main()
