#!/usr/bin/env python3
"""ida_run.py - 走 IDA MCP 跑一段 IDAPython 并打印结果。
用法: python ida_run.py <脚本.py>
脚本里可用全局 `M`（ida_mcp 模块）与 `ev(code)`（py_eval 返回文本）。
"""
import sys, os, json
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ida_mcp as M

M.init()


def ev(code):
    r = M.rpc('tools/call', {'name': 'py_eval', 'arguments': {'code': code}}, rid=9)
    res = (r or {}).get('result', {})
    txt = '\n'.join(c.get('text', '') for c in res.get('content', []))
    if r and 'error' in r:
        return 'ERROR: ' + json.dumps(r['error'], ensure_ascii=False)[:800]
    try:
        j = json.loads(txt)
        parts = []
        if j.get('stdout'):
            parts.append(j['stdout'])
        if j.get('stderr'):
            parts.append('[stderr] ' + str(j['stderr']))
        return '\n'.join(parts)
    except Exception:
        return txt


def tool(name, args):
    r = M.rpc('tools/call', {'name': name, 'arguments': args}, rid=9)
    res = (r or {}).get('result', {})
    out = [c.get('text', '') for c in res.get('content', [])]
    if r and 'error' in r:
        out.append('ERROR: ' + json.dumps(r['error'], ensure_ascii=False)[:800])
    return '\n'.join(out)


if __name__ == '__main__':
    g = {'M': M, 'ev': ev, 'tool': tool, 'json': json}
    exec(open(sys.argv[1], encoding='utf-8').read(), g)
