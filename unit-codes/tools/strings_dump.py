"""Dump printable ASCII strings from a file, with optional min length and limit."""
import sys, re, os

path = sys.argv[1]
minlen = int(sys.argv[2]) if len(sys.argv) > 2 else 5
limit = int(sys.argv[3]) if len(sys.argv) > 3 else 200
data = open(path, 'rb').read()
pat = re.compile(rb'[\x20-\x7e]{%d,}' % minlen)
n = 0
for m in pat.finditer(data):
    s = m.group().decode('ascii')
    print(f'0x{m.start():08x}  {s}')
    n += 1
    if n >= limit:
        break
print(f'-- shown {n} --')
