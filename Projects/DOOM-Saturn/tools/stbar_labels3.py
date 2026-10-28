# Cluster by exact palette indices used by the label glyphs (idx 4-6 look
# like the light gray of the text).
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
    if nm == "PLAYPAL":
        f.seek(off); pal = f.read(768)

def rgb(i):
    return pal[i*3], pal[i*3+1], pal[i*3+2]

for i in (4, 5, 6, 109, 111):
    print("pal[%d] =" % i, rgb(i))

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

def clusters(idxs, y0, y1, need, gap=5):
    cols = []
    for x in range(w):
        n = sum(1 for y in range(y0, y1) if px[x][y] in idxs)
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

# glyph whites: find which indices form text — sample the AMMO area rows 24-29, x 8-42
import collections
c = collections.Counter()
for x in range(0, 130):
    for y in range(22, 31):
        c[px[x][y]] += 1
print("AMMO-area palette hist:", c.most_common(10))

lab = clusters({5, 109, 111}, 20, 32, 2)
print("label clusters (idx 5/109/111, y20-31):", lab)