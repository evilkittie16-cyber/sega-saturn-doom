"""ppmdiff.py a.ppm b.ppm  - count differing pixels and their bounding box / row ranges."""
import sys


def read(p):
    b = open(p, 'rb').read()
    parts, i = [], 0
    while len(parts) < 4:
        while b[i:i + 1].isspace(): i += 1
        if b[i:i + 1] == b'#':
            while b[i:i + 1] != b'\n': i += 1
            continue
        j = i
        while not b[j:j + 1].isspace(): j += 1
        parts.append(b[i:j]); i = j
    w, h = int(parts[1]), int(parts[2])
    return w, h, b[i + 1:i + 1 + w * h * 3]


w, h, a = read(sys.argv[1])
_, _, b = read(sys.argv[2])
rows = {}
for y in range(h):
    for x in range(w):
        k = (y * w + x) * 3
        if a[k:k + 3] != b[k:k + 3]:
            r = rows.setdefault(y, [x, x, 0]); r[0] = min(r[0], x); r[1] = max(r[1], x); r[2] += 1
n = sum(r[2] for r in rows.values())
print('%d differing px of %d' % (n, w * h))
if rows:
    ys = sorted(rows)
    runs, s = [], ys[0]
    for p, q in zip(ys, ys[1:] + [None]):
        if q != p + 1:
            runs.append((s, p)); s = q
    for s, e in runs:
        xs0 = min(rows[y][0] for y in range(s, e + 1)); xs1 = max(rows[y][1] for y in range(s, e + 1))
        print('  rows %d-%d  x %d-%d  px %d' % (s, e, xs0, xs1, sum(rows[y][2] for y in range(s, e + 1))))
