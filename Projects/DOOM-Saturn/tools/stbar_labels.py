# Decode cd/DOOM.WAD's STBAR and report the x-extent of each bright
# label glyph block in the label rows (bottom half of the 32-row bar),
# plus any bright slots in the number rows.
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
        f.seek(off)
        d = f.read(ln)
        break

w, h = struct.unpack("<hh", d[0:4])
print(f"STBAR {w}x{h}")
colofs = struct.unpack("<%di" % w, d[8:8 + 4 * w])
px = [[0] * h for _ in range(w)]
for x in range(w):
    p = colofs[x]
    while True:
        top = d[p]
        if top == 0xFF:
            break
        ln2 = d[p + 1]
        for j in range(ln2):
            px[x][top + j] = d[p + 3 + j]
        p += 4 + ln2

# brightness map per column per row-band
def band_cols(y0, y1, thresh=100):
    cols = []
    for x in range(w):
        n = sum(1 for y in range(y0, y1) if px[x][y] >= thresh)
        cols.append(n)
    return cols

def clusters(cols, need=1):
    out, run = [], None
    for x, n in enumerate(cols):
        if n >= need:
            run = [x, x] if run is None else [run[0], x]
        else:
            if run:
                out.append(tuple(run))
            run = None
    if run:
        out.append(tuple(run))
    merged = []
    for c in out:
        if merged and c[0] - merged[-1][1] <= 4:
            merged[-1] = [merged[-1][0], c[1]]
        else:
            merged.append(c)
    return merged

# label band: find rows with bright text. Print row profile first.
for y0, y1, tag in ((0, 32, "all"),):
    pr = [sum(1 for x in range(w) if px[x][y] >= 100) for y in range(h)]
    print("row bright-count:", pr)

lab = clusters(band_cols(20, 31))
print("label-band clusters (y20-31):", lab)
num = clusters(band_cols(2, 19))
print("number-band clusters (y2-19):", num)