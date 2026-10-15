#!/usr/bin/env python3
"""ida_mcp.py - 直连 IDA Pro MCP 的轻量客户端（无需 WorkBuddy MCP 集成）
用法:
  python ida_mcp.py tools                      # 列出工具
  python ida_mcp.py call <tool> '<json-args>'  # 调用工具
  python ida_mcp.py probe                      # server_health
"""
import json, sys, urllib.request, os

# 端口可用环境变量覆盖（多个 IDA 实例）：IDA_MCP_PORT=13338
PORT = os.environ.get('IDA_MCP_PORT', '13337')
BASE = f'http://127.0.0.1:{PORT}/mcp'
_sid = None


def _parse(raw):
    raw = raw.strip()
    if not raw:
        return None
    if raw.startswith('data:'):
        # SSE: 取最后一条 data
        for line in raw.splitlines()[::-1]:
            if line.startswith('data:'):
                return json.loads(line[5:].strip())
    try:
        return json.loads(raw)
    except Exception:
        return {'raw': raw[:500]}


def rpc(method, params=None, notify=False, rid=1):
    global _sid
    body = {'jsonrpc': '2.0', 'method': method}
    if not notify:
        body['id'] = rid
    if params is not None:
        body['params'] = params
    hdr = {'Content-Type': 'application/json',
           'Accept': 'application/json, text/event-stream'}
    if _sid:
        hdr['Mcp-Session-Id'] = _sid
    req = urllib.request.Request(BASE, data=json.dumps(body).encode(), headers=hdr)
    with urllib.request.urlopen(req, timeout=60) as r:
        sid = r.headers.get('Mcp-Session-Id')
        if sid:
            _sid = sid
        return _parse(r.read().decode('utf-8', 'ignore'))


def init():
    r = rpc('initialize', {'protocolVersion': '2024-11-05', 'capabilities': {},
                           'clientInfo': {'name': 'ra3-tools', 'version': '1'}})
    rpc('notifications/initialized', notify=True)
    return r


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else 'tools'
    init()
    if cmd == 'tools':
        r = rpc('tools/list', rid=2)
        tools = (r or {}).get('result', {}).get('tools', [])
        print(f'tools: {len(tools)}')
        for t in tools:
            print(' -', t.get('name'))
    elif cmd == 'probe':
        r = rpc('tools/call', {'name': 'server_health', 'arguments': {}}, rid=3)
        print(json.dumps(r, ensure_ascii=False, indent=1)[:2000])
    elif cmd == 'call':
        tool = sys.argv[2]
        args = json.loads(sys.argv[3]) if len(sys.argv) > 3 else {}
        r = rpc('tools/call', {'name': tool, 'arguments': args}, rid=4)
        res = (r or {}).get('result', {})
        for c in res.get('content', []):
            print(c.get('text', ''))
        if r and 'error' in r:
            print('ERROR:', json.dumps(r['error'], ensure_ascii=False)[:500])
    else:
        print('unknown cmd')


if __name__ == '__main__':
    main()
