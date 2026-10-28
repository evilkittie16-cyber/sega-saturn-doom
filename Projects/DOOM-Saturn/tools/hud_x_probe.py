# Locate the red HUD digits inside the status-bar band only, normalized
# to the game screen width (bar art spans the full 320 game px).
import sys
from PIL import Image

def analyze(path, label, band_frac, expect_h):
    img = Image.open(path).convert("RGB")
    w, h = img.size
    px = img.load()

    # The bar band: bottom band_frac of the image (bar is 32 of 200/224 rows).
    y0 = int(h * (1 - band_frac))
    y1 = h

    # Game x-extent: columns in the band that are not near-black
    # (Kronos letterboxes; the rerelease shot fills the frame).
    col_has = []
    for x in range(w):
        for y in range(y0, y1, 3):
            r, g, b = px[x, y]
            if r + g + b > 60:
                col_has.append(x)
                break
    gx0, gx1 = (col_has[0], col_has[-1]) if col_has else (0, w)
    gw = gx1 - gx0 + 1

    # Red digit clusters within the band.
    cols = []
    for x in range(w):
        n = 0
        for y in range(y0, y1):
            r, g, b = px[x, y]
            if r > 150 and g < 90 and b < 90:
                n += 1
        cols.append(n)
    cl, run = [], None
    for x, n in enumerate(cols):
        if n >= 3:
            run = [x, x] if run is None else [run[0], x]
        else:
            if run and run[1] - run[0] >= 2:
                cl.append(tuple(run))
            run = None
    if run:
        cl.append(tuple(run))
    merged = []
    for c in cl:
        if merged and c[0] - merged[-1][1] <= 3:
            merged[-1] = [merged[-1][0], c[1]]
        else:
            merged.append(c)

    print(f"== {label}: img {w}x{h}, band y {y0}-{y1}")
    print(f"   game x-extent {gx0}..{gx1} (width {gw})")
    for a, b in merged:
        ga = (a - gx0) * 320.0 / gw
        gb = (b - gx0) * 320.0 / gw
        print(f"   img {a:5d}-{b:5d}   game {ga:6.1f}-{gb:6.1f}  (w {gb-ga:5.1f})")
    print()

cur = r"C:\Users\charl\Desktop\joengine-master\Projects\DOOM-Saturn\screenshots 2\current\Screenshot 2026-09-06 180201.png"
ref = r"C:\Users\charl\Desktop\joengine-master\Projects\DOOM-Saturn\screenshots 2\current\6-9-2026\Screenshot 2026-09-06 071530.png"
analyze(cur, "CURRENT", 32/224.0, 224)
analyze(ref, "REFERENCE", 32/200.0, 200)