"""SAGE / SuperFastHash string hasher (WrathEd2012 SAGE/StringHasher.cs port).

Validates against name/hash pairs taken from the game.
"""
import json, os, struct

M = 0xFFFFFFFF


def _u32(x):
    return x & M


def sage_hash(value):
    """Port of SAGE.StringHasher.Hash (Paul Hsieh's SuperFastHash variant)."""
    b = value.encode('utf-8')
    n = len(b)
    if n == 0:
        return 0
    if n == 10 and b[0] == 0x30 and b[1] == 0x78:      # "0xXXXXXXXX"
        return int(value[2:], 16) & M
    num_blocks = n // 4
    extra = n % 4
    h = _u32(n)
    idy = 0
    for _ in range(num_blocks):
        h = _u32(h + (b[idy + 1] << 8 | b[idy]))
        tmp = _u32((b[idy + 3] << 8 | b[idy + 2]) ^ _u32(h << 5))
        h = _u32(h ^ _u32(tmp << 11))
        h = _u32(h + (h >> 11))
        idy += 4
    if extra == 1:
        h = _u32(h + b[idy])
        h = _u32(_u32(h << 10) ^ h)
        h = _u32(h + (h >> 1))
    elif extra == 2:
        h = _u32(h + (b[idy + 1] << 8 | b[idy]))
        h = _u32(h ^ _u32(h << 11))
        h = _u32(h + (h >> 17))
    elif extra == 3:
        h = _u32(h + (b[idy + 1] << 8 | b[idy]))
        h = _u32(h ^ _u32(_u32(h ^ _u32(b[idy + 2] << 2)) << 16))
        h = _u32(h + (h >> 11))
    # final avalanche
    h = _u32(h ^ _u32(h << 3))
    h = _u32(h + (h >> 5))
    h = _u32(h ^ _u32(h << 2))
    h = _u32(h + (h >> 15))
    h = _u32(h ^ _u32(h << 10))
    return h


if __name__ == '__main__':
    import re, csv, io
    here = os.path.dirname(os.path.abspath(__file__))
    arch = os.path.abspath(os.path.join(here, '..'))

    # 真值 1：运行时实测过的 名字->代码（unit-codes/data/callable_final.tsv）
    pairs = []
    cf = os.path.join(arch, 'data', 'callable_final.tsv')
    if os.path.exists(cf):
        for line in io.open(cf, encoding='utf-8').read().splitlines()[1:]:
            c = line.split('\t')
            if len(c) >= 4 and c[3].startswith('0x'):
                pairs.append((c[2], int(c[3], 16)))

    # 真值 2：修改器头文件里的条目（兼容两代格式）
    hdr = os.path.join(arch, '..', 'ra3-copilot', 'src', 'units_corona.h')
    if os.path.exists(hdr):
        txt = io.open(hdr, encoding='utf-8').read()
        for m in re.finditer(r'"([A-Za-z][A-Za-z0-9_]*)"[^"]*",\s*(?:u8")?[^"]*"?,?\s*(0x[0-9A-Fa-f]+)u', txt):
            pairs.append((m.group(1), int(m.group(2), 16)))
        if not any(pairs):
            for m in re.finditer(r'"([^"]+)",\s*(0x[0-9A-Fa-f]+)u', txt):
                pairs.append((m.group(1), int(m.group(2), 16)))

    # 去重
    seen, uniq = set(), []
    for s, h in pairs:
        if s not in seen:
            seen.add(s)
            uniq.append((s, h))

    ok = sum(1 for s, h in uniq if sage_hash(s.lower()) == h)
    print(f'SAGE hash 复算通过 : {ok}/{len(uniq)}')
    if ok != len(uniq):
        for s, h in uniq:
            if sage_hash(s.lower()) != h:
                print(f'   MISS {s}  expect=0x{h:08X} got=0x{sage_hash(s.lower()):08X}')
                break

    checks = {'AlliedBarracks': 0x509BD329, 'AlliedRefinery': 0x8ECE261C,
              'AlliedAirfield': 0x5B3008B7, 'AlliedPowerPlant': 0x89B86E3D,
              'AlliedMCV': 0x28DA574E, 'CelestialDF41': 0x25241CE6,
              'AlliedAntiNavyShipTech3': 0x9356389F}
    print('-- 抽查（社区《增援代码》/ 日冕 / 用户实测）--')
    for s, h in checks.items():
        g = sage_hash(s.lower())
        print(f'   {s:<26} expect=0x{h:08X} got=0x{g:08X} {"OK" if g == h else "MISS"}')
