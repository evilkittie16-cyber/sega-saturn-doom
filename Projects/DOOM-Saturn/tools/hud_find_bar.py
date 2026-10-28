# Find the exact bar band in the screenshot and crop the right side clean.
from PIL import Image
import os

ROOT = r"C:\Users\charl\Desktop\joengine-master\Projects\DOOM-Saturn"
SHOT = ROOT + r"\screenshots 2\current\Screenshot 2026-09-06 180201.png"

im = Image.open(SHOT).convert("RGB")
W, H = im.size
px = im.load()

# Metal-bar rows: many mid-gray pixels across the full width.
counts = []
for y in range(H - 260, H):
    n = 0
    for x in range(0, W, 4):
        r, g, b = px[x, y]
        if abs(r - g) < 20 and abs(g - b) < 20 and 55 <= r <= 150:
            n += 1
    counts.append((y, n))

# print a compact profile
for y, n in counts:
    if n > W // 8:
        print(y, n)