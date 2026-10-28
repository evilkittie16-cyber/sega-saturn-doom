# Label glyphs are near-white (>190); metal tops out lower. Cluster.
import struct, os

ROOT = r"C:\Users\charl\Desktop\joengine-master\Projects\DOOM-Saturn"
f = open(os.path.join(ROOT, "cd", "DOOM.WAD"), "rb")
magic, numlumps, diroffs = struct.unpack("<4sii", f.read(12))
f.seek(diroffs)
dir_ = []
for i in range(numlumps):
    off, ln, nm = struct.unpack("<ii8s", f.read(16))
    dir_.append((nm.rstrip(b"\x00").decode(), off, ln))
for nm, off, ln in dir_:
    if nm == "STBAR":
        f.seek(off); d = f.read(ln); break

w, h = struct.unpack("<hh", d[0:4])
colofs = struct.unpack("<%di" % w, d[8:8 + 4 * w])
px = [[0] * h for _ in range(w)]
for x in range(w):
    p = colofs[x]
    while True:
        top = d[p]
        if top == 0xFF: break
        ln2 = d[p + 1]
        for j in range(ln2):
            px[x][top + j] = d[p + 3 + j]
        p += 4 + ln2

def clusters(y0, y1, thresh, need, gap=4):
    cols = []
    for x in range(w):
        n = sum(1 for y in range(y0, y1) if px[x][y] >= thresh)
        cols.append(n)
    out, run = [], None
    for x, n in enumerate(cols):
        if n >= need:
            run = [x, x] if run is None else [run[0], x]
        else:
            if run: out.append(tuple(run))
            run = None
    if run: out.append(tuple(run))
    merged = []
    for c in out:
        if merged and c[0] - merged[-1][1] <= gap:
            merged[-1] = [merged[-1][0], c[1]]
        else:
            merged.append(c)
    return merged

print("labels y22-31 thresh190:", clusters(22, 31, 190, 2))
print("labels y22-31 thresh170:", clusters(22, 31, 170, 2))
# where are the baked red '3' digits at the right? find saturated red cols
def redcols(y0, y1):
    out = []
    for x in range(w):
        n = sum(1 for y in range(y0, y1) if px[x][y] > 100)  # palette idx high = red-ish in PLAYPAL
        # crude: use palette index ranges: reds are indices ~160-175
    return out
# palette-index histogram per band
import collections
for y0, y1, tag in ((2, 19, "numband"), (20, 31, "labelband")):
    cnt = collections.Counter()
    for x in range(w):
        for y in range(y0, y1):
            cnt[px[x][y]] += 1
    print(tag, "top palette idx:", cnt.most_common(8))