# Probe the reference screenshot's bar geometry: where does the metal
# bar start/end horizontally, and where are its known art features
# (face recess, ARMS box) in image px?
from PIL import Image

ref = r"C:\Users\charl\Desktop\joengine-master\Projects\DOOM-Saturn\screenshots 2\current\6-9-2026\Screenshot 2026-09-06 071530.png"
img = Image.open(ref).convert("RGB")
w, h = img.size
px = img.load()
print(f"ref {w}x{h}")

# bar band guess: bottom 32/200 rows
ybar = h - 140          # middle of the bar
row = [px[x, ybar][:3] for x in range(w)]

# gray-ness profile: metal bar is desaturated mid-gray
def grayness(c):
    r, g, b = c
    return (r + g + b) / 3.0 - max(r, g, b) + min(r, g, b)

# print luminance transitions along the row
prev = None
for x in range(0, w, 1):
    r, g, b = row[x]
    lum = (r + g + b) / 3
    key = 'D' if lum < 40 else ('M' if lum < 110 else 'B')
    if key != prev:
        print(f"x={x:5d} lum={lum:6.1f} rgb=({r},{g},{b}) -> {key}")
        prev = key