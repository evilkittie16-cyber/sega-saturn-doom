# What's at the right edge of the reference band? And find the bar's
# true vertical band first (bar top = strong horizontal edge).
from PIL import Image

ref = r"C:\Users\charl\Desktop\joengine-master\Projects\DOOM-Saturn\screenshots 2\current\6-9-2026\Screenshot 2026-09-06 071530.png"
img = Image.open(ref).convert("RGB")
w, h = img.size
px = img.load()

for x in (3000, 3050, 3054, 3060, 3100, 3200, 3247):
    r, g, b = px[x, 1609][:3]
    print(f"x={x}: rgb=({r},{g},{b})")

# find bar top: scan rows near bottom for the row where a long run of
# near-constant "metal" appears? Instead: print column 1600 colors every 20 rows
print()
for y in range(1400, 1749, 20):
    r, g, b = px[1600, y][:3]
    print(f"y={y}: rgb=({r},{g},{b})")