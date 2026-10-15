"""Byte-scan BIG archives for known object template name strings.

Files inside BIG4 are stored raw (uncompressed), so a plain byte search works
without extracting. Prints which archive/offset each needle appears at.
"""
import sys, os, glob

ROOT = r'D:\GAME\Game_RTS\Red Alert 3\Data'
needles = [b'CelestialDF41', b'AlliedAirfield', b'SovietBarracks', b'JapanBarracks',
           b'CVSovietBarracks', b'SovietAntiVehicleVehicleTech1', b'CelestialEngineer']

targets = sys.argv[1:] if len(sys.argv) > 1 else sorted(glob.glob(os.path.join(ROOT, '*.big')))
for p in targets:
    if not os.path.isfile(p):
        continue
    data = open(p, 'rb').read()
    hits = {}
    for n in needles:
        c = data.count(n)
        if c:
            i = data.find(n)
            hits[n.decode()] = (c, i)
    if hits:
        print(f'== {os.path.basename(p)} ({len(data)}B)')
        for k, (c, i) in hits.items():
            print(f'     {k:<32} x{c:<5} first@0x{i:x}')
    del data
