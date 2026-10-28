# Reference: cluster the light-gray label glyphs in its bar band
# (y 1470-1749), and likewise the current shot's, in image px.
from PIL import Image

def label_clusters(img, y0, y1, x0, x1, need=3, gap=6):
    px = img.load()
    cols = []
    for x in range(x0, x1):
        n = 0
        for y in range(y0, y1):
            r, g, b = px[x, y][:3]
            if 150 <= r <= 220 and abs(r - g) < 12 and abs(g - b) < 12:
                n += 1
        cols.append(n)
    out, run = [], None
    for i, n in enumerate(cols):
        x = x0 + i
        if n >= need:
            run = [x, x] if run is None else [run[0], x]
        else:
            if run and run[1] - run[0] >= 3:
                out.append(tuple(run))
            run = None
    if run and run[1] - run[0] >= 3:
        out.append(tuple(run))
    merged = []
    for c in out:
        if merged and c[0] - merged[-1][1] <= gap:
            merged[-1] = [merged[-1][0], c[1]]
        else:
            merged.append(c)
    return merged

ref = Image.open(r"C:\Users\charl\Desktop\joengine-master\Projects\DOOM-Saturn\screenshots 2\current\6-9-2026\Screenshot 2026-09-06 071530.png").convert("RGB")
cl = label_clusters(ref, 1650, 1720, 0, 3054)
print("ref label clusters (img px):")
for a, b in cl:
    print(f"  {a:5d}-{b:5d}  /9.544 = {a/9.544:6.1f}-{b/9.544:6.1f}  /8.75-127 = {(a-127)/8.75:6.1f}-{(b-127)/8.75:6.1f}")