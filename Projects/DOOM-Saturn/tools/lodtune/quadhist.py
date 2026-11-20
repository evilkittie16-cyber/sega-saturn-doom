"""quadhist.py vram.bin [...]: distorted-sprite census by texture width and
on-screen width (px per texture column = LOD headroom)."""
import collections, sys
sys.path.insert(0, __file__.rsplit('\\', 1)[0])
from v1cmd import walk

for p in sys.argv[1:]:
    cmds = walk(open(p, 'rb').read())
    h = collections.Counter()
    for c in cmds:
        addr, cm, ctrl, pmod, colr, srca, size, xy = c
        if cm != 2:
            continue
        tw = ((size >> 8) & 0x3F) * 8
        xs = xy[0::2]
        dx = max(xs) - min(xs)
        b = '<=2' if dx <= 2 else '3-4' if dx <= 4 else '5-8' if dx <= 8 else '9-16' if dx <= 16 else '17-32' if dx <= 32 else '>32'
        h[(tw, b)] += 1
    print(p)
    for tw in sorted({k[0] for k in h}):
        row = [h[(tw, b)] for b in ('<=2', '3-4', '5-8', '9-16', '17-32', '>32')]
        print('  tw %3d: total %4d   <=2 %4d  3-4 %4d  5-8 %4d  9-16 %4d  17-32 %4d  >32 %4d' % ((tw, sum(row)) + tuple(row)))
