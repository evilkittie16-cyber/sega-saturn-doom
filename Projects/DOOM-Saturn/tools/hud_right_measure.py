# Measure where the right-side HUD digits actually land in the user's
# in-game screenshot, in GAME coordinates, calibrated from the STBAR
# art's own baked-in landmarks (label column 254-267, slash 292-295).
from PIL import Image
import struct, os

ROOT = r"C:\Users\charl\Desktop\joengine-master\Projects\DOOM-Saturn"
SHOT = ROOT + r"\screenshots 2\current\Screenshot 2026-09-06 180201.png"

# ---- WAD: real RGB of AMMNUM red, STGNUM gray, and the bar labels ----
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
f.close()
pal = lumps["PLAYPAL"]

def patch_pixels(name):
    d = lumps[name]
    w, h = struct.unpack("<hh", d[0:4])
    colofs = struct.unpack("<%di" % w, d[8:8 + 4 * w])
    idxs = set()
    for x in range(w):
        p = colofs[x]
        while True:
            top = d[p]
            if top == 0xFF: break
            ln2 = d[p + 1]
            for j in range(ln2):
                if d[p + 3 + j]: idxs.add(d[p + 3 + j])
            p += 4 + ln2
    return w, h, sorted(idxs)

for nm in ("AMMNUM0", "AMMNUM5", "STGNUM0", "STGNUM2"):
    w, h, idxs = patch_pixels(nm)
    cols = {i: tuple(pal[i*3:i*3+3]) for i in idxs}
    print(nm, "%dx%d" % (w, h), "pal idx", idxs)
    for i, c in sorted(cols.items()):
        print("   idx", i, "rgb", c)

im = Image.open(SHOT).convert("RGB")
W, H = im.size
print("screenshot", im.size)
bar = im.crop((0, H - 157, W, H))
bp = bar.load()
bw, bh = bar.size

# ---- classify screenshot pixels ----
def near(c, t, tol=40):
    return all(abs(c[k] - t[k]) <= tol for k in range(3))

# landmark colors from the WAD decode above will be printed; use generous
# classes here and rely on clustering:
reds, grays, whites = [], [], []
for y in range(bh):
    for x in range(bw):
        r, g, b = bp[x, y]
        if r > 90 and r - g > 50 and r - b > 50:
            reds.append((x, y))
        elif abs(r - g) < 18 and abs(g - b) < 18 and 110 < r < 210:
            grays.append((x, y))
        elif abs(r - g) < 18 and abs(g - b) < 18 and r >= 210:
            whites.append((x, y))

def xclusters(pts, gap=6):
    if not pts: return []
    xs = sorted(p[0] for p in pts)
    cl = [[xs[0], xs[0]]]
    for x in xs[1:]:
        if x - cl[-1][1] <= gap: cl[-1][1] = x
        else: cl.append([x, x])
    return cl

print("red x clusters:", xclusters(reds))
print("white x clusters:", xclusters(whites))

# ---- calibrate: bar art spans game x 0..320. Find bar's screen extent:
# scan the bar band for the leftmost/rightmost columns that are not the
# window background (Kronos chrome is dark but textured; use row variance)
# Simpler: the user's earlier crops showed the game filling the window.
# Calibrate from red+slash instead: print column histograms of "bright"
# pixels (label glyphs are light gray ~ pal 88-94).
lab = []
for y in range(bh):
    for x in range(bw):
        r, g, b = bp[x, y]
        if abs(r - g) < 20 and abs(g - b) < 20 and 150 < r < 230:
            lab.append((x, y))
print("midgray(label) x clusters:", xclusters(lab, gap=8))

# vertical: y clusters of red per x-band, to see the 4 rows
def yclusters(pts, gap=3):
    if not pts: return []
    ys = sorted(p[1] for p in pts)
    cl = [[ys[0], ys[0]]]
    for y in ys[1:]:
        if y - cl[-1][1] <= gap: cl[-1][1] = y
        else: cl.append([y, y])
    return cl

print("red y clusters:", yclusters(reds))
print("white y clusters:", yclusters(whites))