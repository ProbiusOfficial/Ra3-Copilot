"""Strict sequential EA CSF reader (RA3 gamestrings.csf).

Record layout, starting right after the 0x18-byte header:
    b' LBL' <nStrings:u32> <labelLenBytes:u32> <ascii label>
    then nStrings times:
    b' RTS' <valueLen:u32> <value bytes>       (value = UTF-16LE, len in CHARS)
"""
import struct, sys, json

MODE = sys.argv[3] if len(sys.argv) > 3 else 'chars'   # chars | bytes


def parse(path):
    d = open(path, 'rb').read()
    p = 0x18
    out = {}
    bad = 0
    while p < len(d):
        if d[p:p + 4] != b' LBL':
            # resync: find next ' LBL'
            j = d.find(b' LBL', p)
            if j < 0:
                break
            p = j
            bad += 1
        p += 4
        if p + 8 > len(d):
            break
        n, ln = struct.unpack('<II', d[p:p + 8])
        p += 8
        label = d[p:p + ln].decode('ascii', 'replace')
        p += ln
        vals = []
        for _ in range(max(1, n)):
            if d[p:p + 4] != b' RTS':
                j = d.find(b' RTS', p, p + 8)
                if j < 0:
                    break
                p = j
            p += 4
            vl, = struct.unpack('<I', d[p:p + 4])
            p += 4
            nb = vl * 2 if MODE == 'chars' else vl
            raw = d[p:p + nb]
            # RA3 CSF stores values as bitwise-NOT of UTF-16LE
            ch = raw.decode('utf-16-le', 'replace') if MODE == 'bytes' else \
                 ''.join(chr((~struct.unpack_from('<H', raw, k)[0]) & 0xFFFF)
                         for k in range(0, len(raw) - 1, 2))
            vals.append(ch)
            p += nb
        if label:
            out[label] = vals[0] if vals else ''
    return out, bad


if __name__ == '__main__':
    strs, bad = parse(sys.argv[1])
    print(f'mode={MODE} entries={len(strs)} resyncs={bad}')
    names = {k[5:]: v for k, v in strs.items() if k.startswith('NAME:')}
    print(f'NAME: entries={len(names)}')
    for k in list(names)[:20]:
        print(f'   {k:<45} {names[k]}')
    sample = [v for k, v in list(strs.items())[:20]]
    print('sample values:', sample[:8])
    if len(sys.argv) > 2:
        json.dump(names, open(sys.argv[2], 'w', encoding='utf-8'), ensure_ascii=False, indent=1)
        print('wrote', sys.argv[2])
