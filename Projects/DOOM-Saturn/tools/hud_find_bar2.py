# Locate the bar via its pure-black boxes (ARMS box, key boxes), then
# crop the right side precisely and report geometry.
from PIL import Image

ROOT = r"C:\Users\charl\Desktop\joengine-master\Projects\DOOM-Saturn"
SHOT = ROOT + r"\screenshots 2\current\Screenshot 2026-09-06 180201.png"

im = Image.open(SHOT).convert("RGB")
W, H = im.size
px = im.load()

Y0 = H - 320
col_black = []
for x in range(W):
    n = 0
    for y in range(Y0, H):
        r, g, b = px[x, y]
        if r < 12 and g < 12 and b < 12:
            n += 1
    col_black.append((x, n))

# columns with long black runs = box interiors
runs = [x for x, n in col_black if n > 60]
print("black-heavy columns:", runs[:5], "...", runs[-5:] if runs else None)

# group them
def groups(xs, gap=4):
    out = []
    for x in xs:
        if out and x - out[-1][1] <= gap: out[-1][1] = x
        else: out.append([x, x])
    return out

print("column groups:", groups(runs))

# For the first and last group, find the vertical extent of black
for g in groups(runs):
    x = (g[0] + g[1]) // 2
    ys = [y for y in range(Y0, H) if all(c < 12 for c in px[x, y])]
    print("group mid x=%d: black y %d..%d (len %d)" % (x, ys[0], ys[-1], len(ys)))