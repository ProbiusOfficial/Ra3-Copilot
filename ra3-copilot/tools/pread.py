"""ra3 修改器 HTTP 端点的小工具：读内存 / 取 JSON，供离线分析脚本复用。

用法（作为模块）：
    from pread import rd, u32, u16, u8, snap
"""
import json
import re
import struct
import urllib.request

BASE = "http://127.0.0.1:17832"
_LINE = re.compile(r"^([0-9A-Fa-f]{8}):\s*((?:[0-9A-Fa-f]{2}\s*)+)")


def get(path, timeout=10):
    with urllib.request.urlopen(BASE + path, timeout=timeout) as r:
        return r.read()


def text(path, timeout=10):
    return get(path, timeout).decode("utf-8", "replace")


def rd(addr, n=64):
    """按字节读；端点单次固定回 16 字节/行，多行拼接。"""
    out = bytearray()
    pos = addr
    while len(out) < n:
        want = min(n - len(out), 0x400)
        t = text("/read?addr=0x%08X&n=%d" % (pos, want))
        got = 0
        for line in t.splitlines():
            m = _LINE.match(line)
            if m:
                chunk = bytes(int(x, 16) for x in m.group(2).split())
                out += chunk
                got += len(chunk)
        if got == 0:
            break
        pos += got
    return bytes(out[:n])


def u32(addr):
    b = rd(addr, 4)
    return struct.unpack("<I", b)[0] if len(b) == 4 else None


def u16(addr):
    b = rd(addr, 2)
    return struct.unpack("<H", b)[0] if len(b) == 2 else None


def u8(addr):
    b = rd(addr, 1)
    return b[0] if b else None


def f32(addr):
    b = rd(addr, 4)
    return struct.unpack("<f", b)[0] if len(b) == 4 else None


def snap():
    return json.loads(text("/snapshot"))


def cstr(addr, n=64):
    b = rd(addr, n)
    return b.split(b"\x00")[0].decode("latin1", "replace")


def dump(addr, n=64):
    """按 16 字节一行打印，附带 ASCII 列。"""
    b = rd(addr, n)
    for off in range(0, len(b), 16):
        chunk = b[off:off + 16]
        asc = "".join(chr(c) if 32 <= c < 127 else "." for c in chunk)
        print("%08X: %s | %s" % (addr + off, " ".join("%02X" % c for c in chunk), asc))
    return b
