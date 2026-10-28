# Right side of our STBAR: label rows (bright text at x 250-275) and
# slash rows (x 288-300), plus AMMNUM/STGNUM glyph sizes.
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
            px[x][top + j] = d[p + 3 + j]
        p += 4 + ln2

def rows(x0, x1, thresh=140):
    out = []
    for y in range(h):
        n = sum(1 for x in range(x0, x1) if px[x][y] >= thresh)
        out.append(n)
    return out

print("label rows (x252-275):", rows(252, 275))
print("slash rows (x286-300):", rows(286, 300))

# label x extents per label row-band
def cols(y0, y1, thresh=140):
    out = []
    for x in range(245, 320):
        n = sum(1 for y in range(y0, y1) if px[x][y] >= thresh)
        out.append((x, n))
    on = [x for x, n in out if n >= 1]
    return (on[0], on[-1]) if on else None

for y0, y1, tag in ((4, 11, "BULL"), (11, 18, "SHEL"), (18, 25, "ROKT"), (25, 32, "CELL")):
    print(tag, "label x:", cols(y0, y1))

# glyph sizes
for name in ("AMMNUM0", "AMMNUM5", "STGNUM0", "STYSNUM2"):
    ww, hh = struct.unpack("<hh", lumps[name][0:4])
    print(name, ww, "x", hh)