# Detect vertical panel-divider seams in our STBAR art and in the
# reference bar, then report where each number sits inside its panel.
from PIL import Image
import struct

ROOT = r"C:\Users\charl\Desktop\joengine-master\Projects\DOOM-Saturn"

# ---- our art ----
f = open(ROOT + r"\cd\DOOM.WAD", "rb")
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

# seam = column dark across the middle band (metal rows 2..19)
seams = []
run = None
for x in range(w):
    dark = sum(1 for y in range(2, 20) if px[x][y] <= 40)
    if dark >= 14:
        if run is None: run = [x, x]
        else: run[1] = x
    else:
        if run is not None:
            seams.append(tuple(run)); run = None
if run: seams.append(tuple(run))
print("OUR ART dark seams (game px):", seams)

# black box extent (fully black columns)
black = [x for x in range(w) if all(px[x][y] <= 40 for y in range(2, 20))]
print("OUR ART black-box columns:", (black[0], black[-1]) if black else None)

# ---- reference ----
ref = Image.open(ROOT + r"\screenshots 2\current\6-9-2026\Screenshot 2026-09-06 071530.png").convert("RGB")
rp = ref.load()
# bar band y 1470..1749, content x 0..3053, x-scale 9.544
y0, y1 = 1490, 1620   # metal rows (above labels)
seams_r = []
run = None
for x in range(0, 3054):
    dark = sum(1 for y in range(y0, y1, 4) if sum(rp[x, y][:3]) / 3 < 45)
    n = (y1 - y0) // 4
    if dark >= n - 1:
        if run is None: run = [x, x]
        else: run[1] = x
    else:
        if run is not None:
            seams_r.append(tuple(run)); run = None
if run: seams_r.append(tuple(run))
print("REF seams img px:", seams_r)
print("REF seams game px (/9.544):", [(round(a/9.544,1), round(b/9.544,1)) for a, b in seams_r])