"""List / summarize entries of a BIG4 archive.

Usage:
  python big_ls.py <big>                  -> summary by extension + top dir
  python big_ls.py <big> --grep <substr>  -> list matching names
  python big_ls.py <big> --ext xml        -> list names with given extension
"""
import sys, os, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ra3_big4 import parse_big4

big = sys.argv[1]
args = sys.argv[2:]
_, count, _, entries, fsize = parse_big4(big)

if '--grep' in args:
    sub = args[args.index('--grep') + 1].lower()
    hits = [e for e in entries if sub in e[0].lower()]
    print(f'# {count} entries, {len(hits)} match "{sub}"')
    for name, off, size in hits:
        print(f'{size:>10}  {name}')
elif '--ext' in args:
    ext = args[args.index('--ext') + 1].lower().lstrip('.')
    hits = [e for e in entries if e[0].lower().endswith('.' + ext)]
    print(f'# {count} entries, {len(hits)} with .{ext}')
    for name, off, size in hits[:400]:
        print(f'{size:>10}  {name}')
    if len(hits) > 400:
        print(f'... (+{len(hits) - 400} more)')
else:
    print(f'{os.path.basename(big)}: {count} entries, {fsize} bytes')
    ext = collections.Counter()
    top = collections.Counter()
    for name, off, size in entries:
        n = name.replace('\\', '/')
        e = n.rsplit('.', 1)[-1].lower() if '.' in n.rsplit('/', 1)[-1] else '(none)'
        ext[e] += 1
        top[n.split('/', 1)[0] if '/' in n else '(root)'] += 1
    print('\n-- by extension --')
    for e, c in ext.most_common(30):
        print(f'  {c:>7}  .{e}')
    print('\n-- by top-level dir --')
    for t, c in top.most_common(30):
        print(f'  {c:>7}  {t}')
