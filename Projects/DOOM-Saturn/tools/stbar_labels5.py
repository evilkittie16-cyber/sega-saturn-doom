# Labels = light gray glyphs (pal 88/90/92...). Cluster their x extents.
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

GLYPH = {88, 90, 92, 94}
cols = []
for x in range(w):
    cols.append(sum(1 for y in range(22, 32) if px[x][y] in GLYPH))

out, run = [], None
for x, n in enumerate(cols):
    if n >= 2:
        run = [x, x] if run is None else [run[0], x]
    else:
        if run: out.append(tuple(run))
        run = None
if run: out.append(tuple(run))
merged = []
for c in out:
    if merged and c[0] - merged[-1][1] <= 6:
        merged[-1] = [merged[-1][0], c[1]]
    else:
        merged.append(c)
print("label glyph clusters:", merged)
for a, b in merged:
    print(f"  x {a:3d}-{b:3d}  center {(a+b)/2:6.1f}  width {b-a+1}")