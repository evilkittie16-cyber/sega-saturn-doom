# Same measurement, brightness-mapped through PLAYPAL.
import struct

ROOT = r"C:\Users\charl\Desktop\joengine-master\Projects\DOOM-Saturn"
f = open(ROOT + r"\cd\DOOM.WAD", "rb")
magic, numlumps, diroffs = struct.unpack("<4sii", f.read(12))
f.seek(diroffs)
dir_ = []
for i in range(numlumps):
    off, ln, nm = struct.unpack("<ii8s", f.read(16))
    dir_.append((nm.rstrip(b"\x00").decode(), off, ln))
lumps = {}
for nm, off, ln in dir_:
    f.seek(off); lumps[nm] = f.read(ln)
pal = lumps["PLAYPAL"]
lum = [pal[i*3] for i in range(256)]   # grayscale palette: r=g=b

d = lumps["STBAR"]
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
            px[x][top + j] = lum[d[p + 3 + j]]
        p += 4 + ln2

def rows(x0, x1, thresh=140):
    return [sum(1 for x in range(x0, x1) if px[x][y] >= thresh) for y in range(h)]

lr = rows(250, 276)
print("label rows (x250-276):", lr)
sr = rows(284, 300)
print("slash rows (x284-300):", sr)

def cols(y0, y1, thresh=140):
    on = [x for x in range(245, 320) if any(px[x][y] >= thresh for y in range(y0, y1))]
    return (on[0], on[-1]) if on else None

for y0, y1, tag in ((3, 10, "row1"), (10, 17, "row2"), (17, 24, "row3"), (24, 32, "row4")):
    print(tag, "bright x:", cols(y0, y1))